# CHOMPI Firmware Launcher

Turn CHOMPI on and it lights one key for every firmware on the SD card. Press a
key, that firmware starts. Power cycle to come back to the picker.

Stock CHOMPI holds exactly one firmware, because the bootloader installs the
first `.bin` it finds in the card root and then boots it forever. This replaces
that single slot with a menu, and it does so **without modifying the
bootloader** — nothing is ever written to the processor's internal flash, so
there is no DFU step and no way to brick the unit. It installs exactly like any
other firmware update.

## How it works

CHOMPI apps are `BOOT_SRAM` images: the bootloader copies them out of QSPI into
SRAM_EXEC at `0x24000000` and jumps there. Every app runs from the same
address, so where an image was stored is irrelevant — which is what makes it
possible for one app to start another.

The launcher is an ordinary app living in QSPI. On boot it:

1. Mounts the card and scans `/FIRMWARE` for `.bin` files.
2. Lights the white key under each one and waits for a press.
3. Reads the chosen image into SDRAM (64MB available; an image is ~250KB).
4. Validates the image's vector table before committing to it.
5. Tears down the peripherals, then resets into the chosen firmware.

The handover is the delicate part, and it is worth explaining why it looks
the way it does.

The launcher runs from SRAM_EXEC, so the copy would overwrite the code
performing it. A small position-independent routine is relocated into ITCMRAM
at `0x00000000` first — a separate 64K block, untouched by the copy and
executable without any setup (the stock bootloader stages code there too).

That routine does **not** branch into the new image. It copies, then writes
`SCB->AIRCR = 0x05FA0004` and resets the chip. `boot_info` in backup SRAM —
set just before, and undisturbed by the copy — tells the bootloader where to
go. SRAM survives a warm reset, so on the way back up the bootloader's
`startup_process()` finds the request, clears it, and jumps from completely
clean hardware.

This is what the stock bootloader does (`LoadProgramAndJump()` in
`shared/bootloader.cpp`). The direct-jump version is still in that file,
commented out, beside a note that masking interrupts "seem[s] to cause errors
for the target application". Branching means unwinding every peripheral and
DMA stream by hand and getting all of it right; a reset does it atomically.
Handing over by jumping was tried here first and failed three separate ways
before this replaced it.

The trampoline is written as a naked function in pure assembly on purpose. The
compiler must not emit a literal pool or any PC-relative data reference,
because the code runs from an address it was not linked for. Every constant is
built with `movw`/`movt` immediates and every input arrives in a register. It
also touches no stack, so it does not care that its own stack frame is part of
what gets overwritten. Verify after any change:

```bash
arm-none-eabi-objdump -d build/CHOMPI.elf \
  --disassemble='_ZN6chompiL10TrampolineEmmm' | grep 'ldr.*\[pc'   # must be empty
```

It is currently 58 bytes against a 256-byte budget, checked at startup by
`CheckTrampolineFits()`.

## Why the firmwares hide in a subdirectory

The stock bootloader skips directories and never recurses. `/FIRMWARE/*.bin` is
therefore invisible to it, and the launcher in the root is the only thing it
ever installs. QSPI keeps holding the launcher no matter which firmware last
ran, so every power-on returns to the picker.

## One folder per firmware

Stock firmwares keep everything in the card root: TAPE's 168 samples, WAVE's
wavetables, and all three firmwares' `options.json` and `presets.json` in three
mutually incompatible formats. On a shared card that is a contested namespace,
and a *mutable* one — TAPE also writes there while sampling.

It breaks in practice. WAVE scans the root for `.wav`, sorts, and preloads the
first seven; next to TAPE's samples it loads drum hits as wavetables and comes
up silent.

So each firmware now moves into its own folder at startup, immediately after
mounting:

```c
f_chdir("/TAPE");     // or /TEMPO, /WAVE
```

Every path TAPE opens was already relative, so that one call relocates its
samples, its settings, and the files it writes while sampling (`temp_rec.wav`,
`looper.wav`, `chompi_xy.wav`). TEMPO needed its `Chromatic` / `Slice` /
`Buffer` literals made relative too, including the ones it writes to. WAVE
needed the `chdir` plus the scan directory.

**Each falls back to the root when its folder is absent**, so these binaries
still work unchanged on a stock single-firmware card.

Settings therefore need no management: each firmware reads and writes its own,
in its own folder, and they cannot collide. An earlier version of this launcher
shuffled them in and out of the root; that is gone.

## Card layout

```
/CHOMPI.bin            the launcher -- the ONLY .bin in the root
/FIRMWARE/01_TAPE.bin  offered on white key 1
/FIRMWARE/02_TEMPO.bin white key 2
/FIRMWARE/03_WAVE.bin  white key 3
/TAPE/                 TAPE's samples, options.json, presets.json
/TEMPO/                Chromatic/ Slice/ Buffer/, options.json, presets.json
/WAVE/                 wavetables, options.json, presets.json
```

The number in front of the name is the key: `05_TAPE-DEV.bin` is on key 5,
whatever else is on the card, and keys without a firmware stay dark. Files
without a number fill the free keys in name order. Add a firmware by dropping
a `.bin` in `/FIRMWARE`; up to 15 are shown, one per white key. Have it `f_chdir()` into its own folder and it
will never collide with anything else on the card.

`./make-card.sh /Volumes/YOUR_CARD` builds this layout from the factory card
profiles in `firmware/card-profiles`. It only adds and overwrites, never
deletes, and refuses to run if a second `.bin` is sitting in the root where it
would race the launcher for the bootloader's attention.

## Sending a firmware over USB MIDI

While the launcher is up -- picker showing, or parked on any fault -- it also
listens on USB MIDI. Send it an image and a slot number, and it stores the
image in that slot on the card and starts it:

```bash
./midi-send.py --slot 5 ../chompi-tape/code/src/build/CHOMPI.bin
```

The keybed fills in cyan as the image arrives, then amber as it is written to
`/FIRMWARE/05_TAPE.bin`, then white as it is read back to start. Sending to
slot 5 again replaces it, so a work-in-progress build keeps one slot instead
of piling up. Whatever was in the slot before goes, including a stock
firmware: `--slot 1` on a standard card replaces `01_TAPE.bin`.

The name defaults to the project folder the image was built in
(`chompi-tape` -> `TAPE`); `--name` overrides it.

The image is written to a temporary file and read back before the slot's old
file is removed. Once stored, it is started exactly as if its key had been
pressed: read off the card into the same buffer, handed over by the same
`ChainLoad()`.

That detour through the card is deliberate. An earlier version started the
uploaded image straight from memory, and TAPE then froze at startup, while
the very same bytes started fine from the card. The cause was never found.
Going through the card makes an uploaded firmware indistinguishable from a
picked one, and that route has not failed since.

It also works with an empty `/FIRMWARE` or a card inserted after power-on.
Without a card it reports `NO_CARD` and nothing happens.

`midi-send.py` is Linux only and needs nothing beyond Python 3. For macOS and
Windows there is a web page that does the same in Chrome or Edge:
https://ugrossek.github.io/CHOMPI/ (source in [ugrossek/CHOMPI](https://github.com/ugrossek/CHOMPI/tree/midi-firmware-load/docs)). The protocol is
specified in [PROTOCOL.md](PROTOCOL.md), for anyone writing another client.

USB only appears once the launcher has taken the data lines back from the
MP2722 charger, which TAPE does as well: they are switched between the two, and
the charger borrows them to identify the port.

## Building

Needs ARM's **official** GCC 10.3-2021.10 — not xPack's rebuild of the same
version, which emits larger code and fails to link TAPE.

```bash
PATH=~/dev/chompi/toolchain/gcc-arm-none-eabi-10.3-2021.10/bin:$PATH make -j8
```

Output is `build/CHOMPI.bin`, which goes in the card root.

`libDaisy` and `DaisySP` are used from the upstream checkout
(`../../CHOMPI-official/firmware/chompi-wave/code/libs`) rather than
duplicated; they ship prebuilt, and rebuilding them against a different
compiler is what causes the SD-card trouble CHOMPI Club warns about. Override
with `make CHOMPI_LIBS=/path/to/libs`.

## LED feedback

| Pattern | Meaning |
|---|---|
| White keys breathing | Picker. One lit key per firmware found. |
| Chosen key flashes 3×, then stays lit | Selection acknowledged, loading. |
| Keybed filling left to right | Read progress. Distinguishes a slow read from a dead unit. |
| Slow **red** pulse | No card, or it would not mount. |
| Slow **amber** pulse | Card mounted, `/FIRMWARE` is empty. |
| Slow **magenta** pulse | The image would not read off the card. |
| Slow **blue** pulse | Image read, but its vector table was rejected. |
| Slow **white** pulse | Trampoline does not fit its landing site (should be impossible). |
| Keybed filling in **cyan** | Firmware arriving over USB MIDI. |
| Keybed filling in **amber** | That firmware being written to its slot on the card. |

On any fault the unit parks there rather than jumping into nothing, and the
log is committed to the card first.

## Hard-won details

Four things cost real debugging time. All of them are load-bearing.

**Every `FIL` must be static, never a stack local.** A `FIL` carries its own
512-byte sector buffer, and FatFS hands that buffer straight to the SD
driver's DMA. The stack is in DTCMRAM (`_estack = 0x20020000`), and DTCM is
not reachable by DMA on the STM32H7. A `FIL` on the stack silently breaks
every read *and* write through it, while directory operations keep working,
because those use the `FATFS` object's own window buffer — a global. Every
`FIL` in the stock firmware is a class member for exactly this reason.

**SDMMC cannot DMA into SDRAM either.** The image is read through a bounce
buffer in internal RAM, as `FileStreamingManager` does with its
`workspace_buffer`.

**`boot_info` needs a linker region CHOMPI's scripts do not define.** libDaisy
declares it `__attribute__((section(".backup_sram")))`, but the stock app
linker scripts have no `BACKUP_SRAM` region, so the section silently falls
into `.data` and `boot_info` lands at an SRAM address the bootloader never
reads. `chompi_sram.lds` here adds the region at `0x38800000`. Check it after
any linker change:

```bash
arm-none-eabi-nm build/CHOMPI.elf | grep boot_info   # must be 38800000
```

**The log must not truncate itself.** `LogFlush()` deliberately avoids
`FA_CREATE_ALWAYS`, which truncates on open — so a flush whose write then
fails leaves an empty file. An earlier version did that and repeatedly
destroyed the evidence it was collecting.

## Status

**Working on hardware.** All three stock firmwares (TAPE, TEMPO, WAVE) boot
from one card, and per-firmware settings survive switching — confirmed by
saving a TAPE preset and finding it intact after a round trip through another
firmware.

**USB upload working on hardware.** Stock TAPE sent with `midi-send.py` was
stored, started, and afterwards offered in the picker; sending again to the
same slot replaced it rather than adding one. Card write plus readback takes
140–290 ms for a 240K image.

A healthy boot reads like this. Note the read time — that is the whole image:

```
[    114] f_mount("0:/") -> 0
[    115] directory walk saw 9 entries, accepted 3
[    116] ready -- picker up with 3 slot(s)
[  21382] f_open("FIRMWARE/01_TAPE.bin") -> 0
[  21486] read loop: 240520/240520 bytes in 470 chunks, 104 ms
[  21486] vector table: MSP=0x20020000 entry=0x24001901
[  21542] handing over to 0x24001901 -- goodbye
```

The very first start after the bootloader has installed a new launcher has
been seen to hang with the LEDs frozen, before the launcher logged anything.
A power cycle cleared it, and it has not recurred on later starts. The same
happened once with a TAPE build, so it looks like a property of the first
start after flashing rather than of the launcher.

Recovery if a handover ever misbehaves: power cycle. The launcher is still in
QSPI and nothing was written to internal flash. The jump request is one-shot —
`startup_process()` clears it before acting — so a stale request cannot send
the unit into dead SRAM on the next power-up. If QSPI itself is disturbed,
reflash via https://flash.daisy.audio.
