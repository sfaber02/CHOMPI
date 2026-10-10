# CHOMPI Launcher USB-MIDI Upload Protocol

**Protocol version 1.** Launcher v1.1 brought PING, BEGIN, DATA and END;
v1.5 adds the features below (LIST, CLEAR, STAY, the launcher's own update)
and RUN. Additions keep version 1: clients find them in PING's features byte.

This describes how a host program sends a firmware image to the CHOMPI
launcher over USB MIDI and stores it in a slot on the SD card. It is meant
for anyone writing a client: `midi-send.py` here, a browser page using Web
MIDI, or a native app on any platform.

## Overview

1. The client sends the whole image, the slot number and a name.
2. The launcher checks the CRC and the image, then writes it to
   `/FIRMWARE/NN_NAME.bin` on the card, replacing whatever was in that slot.
3. The launcher boots the new firmware from the card, as if the user had
   picked it in the menu.

Sending again to the same slot replaces the firmware there. That is the
intended develop-test-debug loop: one slot for your work-in-progress build,
overwritten on each send.

Newer launchers can also set up several slots in one go: the client lists
what is on each key, clears keys, and stores firmwares without starting them
(see Features). The launcher then stays in the picker throughout.

## When the launcher listens

The launcher listens while it is running, i.e. while the picker is showing or
while it is parked on a fault pulse. Once a firmware has started, the
launcher is gone. Power-cycle CHOMPI to return to it.

It enumerates as a class-compliant USB MIDI device whose name contains
`CHOMPI`. No driver is needed.

## Framing

Every message in both directions is one SysEx message:

```
F0 7D 43 48 <cmd> <payload...> F7
```

- `7D` is the MIDI manufacturer ID reserved for non-commercial and
  educational use.
- `43 48` (ASCII "CH") distinguishes this protocol from anyone else using
  `7D`. The launcher silently ignores SysEx without this header.
- `<cmd>` is the command byte. A reply carries the command it answers OR'd
  with `0x40`, so `02 BEGIN` is answered by `42`.
- Every payload byte is 7-bit (`00`–`7F`), as MIDI requires.
- No message is longer than 320 bytes.

### Numbers

Multi-byte numbers are sent 7 bits per byte, least significant group first:

| Type | Bytes | Range |
|---|---|---|
| `u7` | 1 | 0 – 127 |
| `u28` | 4 | 0 – 268,435,455 |
| `u35` | 5 | 0 – 2³⁵−1 (holds a full 32-bit CRC) |

Example: 240520 = `0x3AB88` → `08 57 0E 00`.

### Packed data

Image bytes are 8-bit, so `DATA` packs them. The raw bytes are split into
groups of up to 7. Each group is sent as one byte holding the groups' top
bits (bit *i* = bit 7 of byte *i*), followed by the bytes with bit 7 cleared.

```
raw:    00 00 02 20 01 19 00 | 24
packed: 00 00 00 02 20 01 19 00 | 00 24
```

*n* raw bytes become *n* + ⌈*n*/7⌉ packed bytes.

## Conversation

The client sends one message and waits for its reply before sending the
next (stop-and-wait). There is never more than one message in flight.

```
client                         launcher
  PING                 ──────►
                       ◄──────  41  version, max_chunk, slots
  BEGIN size,slot,name ──────►
                       ◄──────  42  status
  DATA  offset, bytes  ──────►     ┐
                       ◄──────  43  status, received      │ until done
  ...                              ┘
  END   crc32          ──────►     (checks, writes the card)
                       ◄──────  44  status
                                   (on OK: boots the firmware)
```

Starting a firmware that is already on the card is a single exchange:

```
client                         launcher
  RUN   slot           ──────►
                       ◄──────  45  status
                                   (on OK: boots the firmware)
```

## Commands

### `01` PING

Payload: none.

Reply `41`:

| Field | Type | Meaning |
|---|---|---|
| status | `u7` | always `0` |
| version | `u7` | protocol version, `1` for this document |
| max_chunk | `u28` | most raw bytes one `DATA` may carry (2048 from launcher v1.5; 256 before). Clients may send less |
| slots | `u7` | highest slot number accepted (currently 15) |
| features | `u7` | optional, see Features. Missing on older launchers: treat as `0` |
| launcher_version | 3 × `u7` | optional: major, minor, patch of the launcher itself. Missing on older launchers |

PING is safe at any time and does not affect an upload in progress. Clients
should use it to find the launcher and should refuse to continue if
`version` is one they do not know. They should accept a longer reply than
they expect and ignore what they do not know.

### `02` BEGIN

| Field | Type | Meaning |
|---|---|---|
| size | `u28` | image size in bytes |
| slot | `u7` | 1 – `slots`, or 127 for a new launcher (`LAUNCHER`). 0 is reserved (see below) |
| name_len | `u7` | 1 – 16 |
| name | `name_len` bytes | ASCII, see Names |

Reply `42`: status (`u7`).

BEGIN starts a new upload and discards any previous one, complete or not.
It does not touch the card. A client can always recover by starting over
with BEGIN.

Fails with `TOO_BIG` if `size` is under 8 bytes or exceeds what the
launcher can hold (currently 512 KB), `BAD_SLOT` if the slot is out of
range, and `BAD_NAME` if the name breaks the rules.

### `03` DATA

| Field | Type | Meaning |
|---|---|---|
| offset | `u28` | where these bytes go in the image |
| data | packed | 1 – `max_chunk` raw bytes, packed |

Reply `43`:

| Field | Type | Meaning |
|---|---|---|
| status | `u7` | |
| received | `u28` | contiguous bytes received so far, from offset 0 |

`offset` may be anything up to `received`, never beyond (no gaps). That
makes a resend harmless: if a reply gets lost, the client sends the same
DATA again. The client should continue from `received` in the reply, not
from its own count.

Fails with `NOT_STARTED` without a prior BEGIN, `BAD_OFFSET` if `offset` >
`received`, and `TOO_BIG` if the chunk exceeds `max_chunk` or would run past
`size`.

### `04` END

| Field | Type | Meaning |
|---|---|---|
| crc32 | `u35` | CRC-32 of the whole image |
| flags | `u7` | optional, only if the launcher has `STAY` (see Features). Bit 0: store only, do not start |

The CRC is the standard IEEE 802.3 / zlib / PNG CRC-32 (reflected,
polynomial `0xEDB88320`, initial value and final XOR `0xFFFFFFFF`), as
returned by Python's `zlib.crc32()`.

Reply `44`: status (`u7`).

The launcher then does the following, in order. The reply comes only after
the last step, so it is final:

1. Checks that all `size` bytes arrived (`INCOMPLETE`).
2. Checks the CRC (`BAD_CRC`).
3. Checks the image (`BAD_IMAGE`, see Image requirements).
4. Writes the slot file and verifies it (`NO_CARD`, `WRITE_FAILED`,
   `CARD_FULL`).

On `OK`, the launcher boots the firmware right after replying, unless
the flags asked it to store only (then see `STAY` below). The client
should then close the MIDI port. The launcher is gone, and the USB device
disappears or changes as the new firmware starts.

**Resend END once if no reply comes within 10 s.** That covers an END that
got lost on the way. If it was the reply that got lost, the launcher has
already started the firmware and the resend goes unanswered too. No reply
to either therefore means "unknown": the firmware was most likely written
and started. The client should say so rather than claim a failure.

On any failure, nothing on the card has changed and the upload is still in
memory. Problems with the image (`BAD_CRC`, `BAD_IMAGE`) need a new upload.
Problems with the card (`NO_CARD`, `WRITE_FAILED`, `CARD_FULL`) can be fixed
(insert the card, make room) and END sent again. A write that fails part way
leaves nothing behind on the card.

## Features

PING's last byte says what the launcher can do beyond the upload above. A
client uses a feature only if its bit is set; an older launcher answers the
commands below with `BAD_MESSAGE`, and END with a flags byte the same way.

| Bit | Name | Meaning |
|---|---|---|
| 0 | `LIST` | `06 LIST` is there |
| 1 | `CLEAR` | `07 CLEAR` is there |
| 2 | `STAY` | END takes the flags byte |
| 3 | `LAUNCHER` | BEGIN to slot 127 updates the launcher itself |
| 4 | `RUN` | `05 RUN` is there (launchers with RUN but no features byte have it too) |

The test launchers 1.4.0 – 1.4.2 had LIST and CLEAR at `05` and
`06`; a client that wants to talk to them can tell them apart by
`launcher_version` (missing on 1.4.0, which already had the features byte).

### `STAY`: store without starting

With bit 0 of END's flags set, the launcher stores the firmware as usual and
then stays where it is. The picker shows the new key, and the launcher keeps
listening, so the client can send the next firmware right away. If a fault
was showing (no card, nothing on the card), the picker comes up as soon as
there is a firmware to offer.

An END resent after a lost reply stores the same image again, which is
harmless.

### `06` LIST

| Field | Type | Meaning |
|---|---|---|
| slot | `u7` | 1 – `slots` |

Reply `46`:

| Field | Type | Meaning |
|---|---|---|
| status | `u7` | |
| slot | `u7` | the slot asked about |
| name_len | `u7` | 0 – 40; 0 means the key is empty |
| name | `name_len` bytes | the file's name in `/FIRMWARE`, e.g. `05_TAPE-DEV.bin` |
| size | `u28` | its size in bytes |

This is what the picker shows on that key, so it includes files without a
number that filled a free key (`HMMM.bin`). Longer names are cut to 40
characters, and anything outside printable ASCII comes as `?`. On failure
(`BAD_SLOT`, `NO_CARD`) the reply is the status only.

### `07` CLEAR

| Field | Type | Meaning |
|---|---|---|
| slot | `u7` | 1 – `slots` |
| name_len | `u7` | optional, 1 – 40 |
| name | `name_len` bytes | the file LIST reported on that key |

Reply `47`:

| Field | Type | Meaning |
|---|---|---|
| status | `u7` | |
| slot | `u7` | the slot asked about |
| removed | `u7` | `1` if something was removed, `0` if not |

Removes every `NN_*` file of that slot from `/FIRMWARE`, and the file
without a number sitting on that key if there is one, i.e. whatever LIST
reported. An empty key is not an error. Afterwards a file without a number
may move onto the freed key; LIST again to see the result.

With a name, the key is emptied only if LIST would report exactly that name
on it; otherwise nothing is removed and the reply is `OK` with `removed` 0.
Clients should always send the name. A CLEAR resent after a slow reply would
otherwise also remove a file without a number that moved onto the key in the
meantime. On failure (`BAD_SLOT`, `NO_CARD`, `WRITE_FAILED`) the reply is the
status only.

### `LAUNCHER`: updating the launcher

BEGIN with slot **127** (out of the range of keys; slot 0 is reserved) sends
a new launcher instead of a firmware. The name is ignored but must follow
the name rules; use `LAUNCHER`. DATA as usual. At END, besides the usual
checks, the launcher:

1. Checks that the image is a launcher: it must contain the text
   `CHOMPI-LAUNCHER ` followed by its version, e.g. `CHOMPI-LAUNCHER 1.5.0`
   (`NOT_LAUNCHER`). Launchers carry this tag from v1.5 on (and the test builds from 1.4.1).
2. Refuses if the card root holds another `.bin` besides `CHOMPI.bin`
   (`OTHER_BIN`): the bootloader installs the first `.bin` it finds, so the
   new launcher might never be installed.
3. Writes the image to a temporary file in `/FIRMWARE` and reads it back.
4. Renames `/CHOMPI.bin` to `/CHOMPI.old` (replacing an older one) and the
   temporary file to `/CHOMPI.bin`.
5. Replies `OK`, then restarts. The bootloader finds a `CHOMPI.bin` that
   differs from what it holds and installs it (a rainbow on the keys), then
   starts it. The USB device goes away and comes back with the new launcher;
   PING it to check `launcher_version`.

The STAY flag is ignored. Any failure leaves the card and the running
launcher as they were.

Power loss is safe at every step: until the bootloader has installed the new
`CHOMPI.bin`, it still holds the old launcher and starts that; if it is cut
off while installing, the new file is still on the card and it tries again.
If a new launcher turns out not to work, rename `CHOMPI.old` back to
`CHOMPI.bin` on a computer.

### Setting up several slots

1. PING, and check `features`.
2. LIST every slot to show the user what is there now.
3. For each change: CLEAR, or BEGIN / DATA / END with `STAY`.
4. LIST again to confirm.

Storing over a slot does not touch a file without a number that was sitting
on that key: it moves to the next free key. Clients that want it gone should
CLEAR the slot first.

### `05` RUN

Starts the firmware already in a slot, exactly as pressing its key would.
Nothing is sent and nothing on the card changes. This lets a client switch
between firmwares without anyone at the panel.

| Field | Type | Meaning |
|---|---|---|
| slot | `u7` | 1 – `slots` |

Reply `45`: status (`u7`).

Fails with `BAD_SLOT` if the slot is out of range or has no firmware in it,
and `BAD_MESSAGE` if the payload is not one byte. On `OK` the launcher flashes
the slot's key, reads the firmware off the card and boots it, as for a key
press; the client should close the MIDI port, as after END.

RUN discards an upload in progress, since starting a slot reads it into the
same buffer.

Launchers from before RUN answer it with `BAD_MESSAGE`, like any unknown
command, so a client can tell them apart without a new protocol version.
Launchers with a features byte also say so in bit 4.
If the reply is lost, a resend goes unanswered because the firmware has
already started; no reply therefore most likely means it did.

## Status codes

| Code | Name | Meaning |
|---|---|---|
| 0 | `OK` | |
| 1 | `BAD_MESSAGE` | malformed message or unknown command |
| 2 | `BAD_OFFSET` | DATA would leave a gap |
| 3 | `TOO_BIG` | image or chunk exceeds the limits |
| 4 | `BAD_CRC` | CRC does not match the data received |
| 5 | `BAD_IMAGE` | image failed validation (not a CHOMPI app) |
| 6 | `NOT_STARTED` | DATA or END without a BEGIN |
| 7 | `INCOMPLETE` | END before every byte arrived |
| 8 | `BAD_SLOT` | slot is 0 or above `slots`; for RUN also an empty slot |
| 9 | `BAD_NAME` | name is empty, too long, or has a forbidden character |
| 10 | `NO_CARD` | no SD card, or it would not mount |
| 11 | `WRITE_FAILED` | the card refused the write, or the readback mismatched |
| 12 | `CARD_FULL` | no room on the card for the image |
| 13 | `NOT_LAUNCHER` | slot 127, but the image has no launcher tag |
| 14 | `OTHER_BIN` | slot 127, but another `.bin` in the card root would be installed instead |

Clients should show unknown codes as a number rather than fail, since later
versions may add more.

## Slots and names

The image is written to:

```
/FIRMWARE/NN_NAME.bin
```

- **`NN`** is the slot number from BEGIN, written as two digits (`05`).
- **`NAME`** is the name from BEGIN.

So slot 5 with name `TAPE-DEV` becomes `/FIRMWARE/05_TAPE-DEV.bin`.

**Writing a slot replaces every file in `/FIRMWARE` whose name starts with
`NN_`**, whatever comes after the prefix. That includes the stock
firmwares. Sending to slot 1 on a standard card replaces `01_TAPE.bin`. A
client should make this visible to the user before sending.

The write is done safely. The image goes to `/FIRMWARE/upload.tmp`, which is
read back and compared. Only then is the old slot file removed and the new
one renamed into place. If power is lost while writing, the old firmware is
still there. If it is lost in the short moment between removing the old file
and the rename, the new image is still on the card as `upload.tmp`.

**Slot *N* is white key *N* in the launcher's picker.** The launcher puts
every `NN_*.bin` on key `NN`; keys without a firmware stay dark, and files
without a number fill the free keys.

### Name rules

- 1 to 16 characters.
- Only `A`–`Z`, `0`–`9`, `-` and `_`.
- Lowercase letters are rejected, not converted, so a client never sees a
  name other than the one it sent. Clients should uppercase before sending.

A good default is the name of the firmware's project folder, e.g. `TAPE`
from `chompi-tape/`. Every build is called `CHOMPI.bin`, so the file name is
no help.

### Slot 0 (reserved)

Slot 0 is reserved for letting the user pick the slot on CHOMPI itself. That
is not implemented, and BEGIN with slot 0 currently fails with `BAD_SLOT`.
If it is added, END's reply would wait for the user's choice, and a status
for "the user cancelled" would be added.

## Image requirements

The launcher accepts only images that look like a CHOMPI app linked to run
from SRAM at `0x24000000` (the `BOOT_SRAM` layout every stock firmware
uses):

- Size from 8 bytes up to 512 KB.
- Word 0 (initial stack pointer) lies in DTCM (`0x20000000`–`0x20020000`) or
  AXI SRAM (`0x24000000`–`0x24080000`).
- Word 1 (reset vector) is a Thumb address (odd) inside the image as loaded
  at `0x24000000`.

This catches sending the wrong file. It does not prove that the firmware
works.

## Data folders

The protocol does not decide where a firmware keeps its samples and
settings. Each firmware decides that itself: TAPE uses `/TAPE`, TEMPO
`/TEMPO` and WAVE `/WAVE`, or the card root if that folder is missing.

So a development build of TAPE in slot 5 shares `/TAPE` with the stock TAPE
in slot 1. Keep that in mind when testing changes that write presets or
samples.

## Timing

| | |
|---|---|
| reply to PING, BEGIN, DATA | expect within 300 ms. Resend after that, up to ~10 tries |
| reply to END | allow 10 s (it writes and verifies ~250 KB on the card) |
| reply to LIST, CLEAR | allow 2 s (they may have to mount the card first) |
| reply to RUN | expect within 300 ms. Resend once at most; no reply most likely means it started |
| gaps between messages | keep below 3 s, or the launcher may treat the upload as abandoned |

A 240 KB image takes about 1–5 s to transfer, depending on the host's MIDI
round-trip time, plus the card write.

## Example

The bytes of a tiny 10-byte image, sent in two chunks to slot 5 as
`TAPE-DEV`. This shows the encoding only. The launcher would reject this
image with `BAD_IMAGE`, because its reset vector points outside it.

```
image: 00 00 02 20 01 19 00 24 FF 80      crc32 = 0x8966E0AF

→ F0 7D 43 48 01 F7                                         PING
← F0 7D 43 48 41 00 01 00 02 00 00 0F F7                    v1, 256-byte chunks, 15 slots

→ F0 7D 43 48 02 0A 00 00 00 05 08 54 41 50 45 2D 44 45 56 F7
                                                            BEGIN 10 bytes, slot 5, "TAPE-DEV"
← F0 7D 43 48 42 00 F7                                      OK

→ F0 7D 43 48 03 00 00 00 00 00 00 00 02 20 01 19 00 00 24 F7
                                                            DATA offset 0, 8 bytes
← F0 7D 43 48 43 00 08 00 00 00 F7                          OK, received 8

→ F0 7D 43 48 03 08 00 00 00 03 7F 00 F7                    DATA offset 8, FF 80
← F0 7D 43 48 43 00 0A 00 00 00 F7                          OK, received 10

→ F0 7D 43 48 04 2F 41 1B 4B 08 F7                          END crc 0x8966E0AF
← F0 7D 43 48 44 05 F7                                      BAD_IMAGE
```

## Notes for client authors

- **Web MIDI** (Chrome, Edge): request access with
  `navigator.requestMIDIAccess({ sysex: true })`. The browser asks the user
  for permission. Send each message whole with `output.send([...])`.
- **Windows (WinMM)**: send SysEx with `midiOutLongMsg`. Most Windows MIDI
  drivers allow only one program to have a port open, so close DAWs first.
- **Linux**: `midi-send.py` writes to the ALSA raw MIDI node
  (`/dev/snd/midiC*D*`) directly.
- Find the device by a port name containing `CHOMPI`, then confirm with
  PING.
- Ignore any incoming message that is not a reply to what you just sent.
- A reply can arrive late, after you have already resent. The launcher then
  answers the resend too, and that second reply turns up while you wait for
  the next message's. For LIST and CLEAR, check the slot in the reply and skip
  replies for another slot. For DATA, continue from `received` as described
  above.
