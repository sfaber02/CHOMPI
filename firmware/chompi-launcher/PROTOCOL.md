# CHOMPI Launcher USB-MIDI Upload Protocol

**Status: draft, protocol version 1.** Nothing has been released yet, so this
is the first version. It is not the one `midi_upload.h` currently implements.

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

## Commands

### `01` PING

Payload: none.

Reply `41`:

| Field | Type | Meaning |
|---|---|---|
| status | `u7` | always `0` |
| version | `u7` | protocol version, `1` for this document |
| max_chunk | `u28` | most raw bytes one `DATA` may carry (currently 256) |
| slots | `u7` | highest slot number accepted (currently 15) |

PING is safe at any time and does not affect an upload in progress. Clients
should use it to find the launcher and should refuse to continue if
`version` is one they do not know.

### `02` BEGIN

| Field | Type | Meaning |
|---|---|---|
| size | `u28` | image size in bytes |
| slot | `u7` | 1 – `slots`. 0 is reserved (see below) |
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

The CRC is the standard IEEE 802.3 / zlib / PNG CRC-32 (reflected,
polynomial `0xEDB88320`, initial value and final XOR `0xFFFFFFFF`), as
returned by Python's `zlib.crc32()`.

Reply `44`: status (`u7`).

The launcher then does the following, in order. The reply comes only after
the last step, so it is final:

1. Checks that all `size` bytes arrived (`INCOMPLETE`).
2. Checks the CRC (`BAD_CRC`).
3. Checks the image (`BAD_IMAGE`, see Image requirements).
4. Writes the slot file and verifies it (`NO_CARD`, `WRITE_FAILED`).

On `OK`, the launcher boots the firmware right after replying. The client
should then close the MIDI port. The launcher is gone, and the USB device
disappears or changes as the new firmware starts.

**Resend END once if no reply comes within 10 s.** That covers an END that
got lost on the way. If it was the reply that got lost, the launcher has
already started the firmware and the resend goes unanswered too. No reply
to either therefore means "unknown": the firmware was most likely written
and started. The client should say so rather than claim a failure.

On any failure, nothing on the card has changed and the upload is still in
memory. Problems with the image (`BAD_CRC`, `BAD_IMAGE`) need a new upload.
Problems with the card (`NO_CARD`, `WRITE_FAILED`) can be fixed (insert the
card) and END sent again.

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
| 8 | `BAD_SLOT` | slot is 0 or above `slots` |
| 9 | `BAD_NAME` | name is empty, too long, or has a forbidden character |
| 10 | `NO_CARD` | no SD card, or it would not mount |
| 11 | `WRITE_FAILED` | the card refused the write, or the readback mismatched |

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
