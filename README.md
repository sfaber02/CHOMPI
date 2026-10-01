# CHOMPI — Open Source

> ### This fork adds a multi-firmware launcher
>
> Run TAPE, TEMPO and WAVE from **one SD card**. Power on and CHOMPI lights one
> key per firmware — press a key, that firmware starts. Power cycle to come
> back to the picker.
>
> No bootloader modification, and nothing is ever written to the processor's
> internal flash. It installs like any ordinary firmware update, and swapping
> back to a stock card returns you to normal.
>
> - **[Download a ready-to-use card image](../../releases/latest)** — unzip to a FAT32 card and go
> - [`firmware/chompi-launcher`](firmware/chompi-launcher/) — source, and how it works
>
> The three stock firmwares here are patched to keep their samples and settings
> in their own folder (`/TAPE`, `/TEMPO`, `/WAVE`) instead of the card root, so
> they can share a card. Each falls back to the root if its folder is missing,
> so the binaries still work on a stock single-firmware card.
>
> A community modification. Not an official CHOMPI Club release — everything
> below is their original README.
>
> **Use at your own risk.** This is unofficial software provided as-is, with no
> warranty. It is built to be safe: it never writes to the processor's internal
> flash, where the bootloader lives, so it should not be able to leave your unit
> unbootable, and putting a stock card back in returns you to normal. If
> something ever does go wrong, the Daisy's USB DFU mode is in factory ROM and
> cannot be overwritten — <https://flash.daisy.audio> will recover the board.

---

**CHOMPI** is a quirky chromatic sampler and tape-music instrument by
[CHOMPI Club](https://www.chompiclub.com).

This repo contains all of the production files, both hardware and firmware, that make up the CHOMPI Sampler.

---

## What's here

| | |
|---|---|
| [`firmware/chompi-launcher`](firmware/chompi-launcher/) | **Added by this fork.** A launcher that boots any of the firmwares in `/FIRMWARE` from a key press, plus `make-card.sh` to build the card. |
| [**Firmware — Start Here**](firmware/README.md) | Quick instructions for setting up your development environment, building the firmware, and loading it onto your CHOMPI. |
| [`firmware/chompi-wave`](firmware/chompi-wave/) | **WAVE 1.0**, a wavetable synth firmware that doubles as a starting point for anyone writing their own firmware. |
| [`firmware/chompi-tempo`](firmware/chompi-tempo/) | **TEMPO 1.0**, a pattern generator firmware — the counterpart to TAPE. |
| [`firmware/chompi-tape`](firmware/chompi-tape/) | **TAPE 2.0**, the sampler firmware every CHOMPI ships with. |
| [`firmware/chompi-bootloader-v6.4-beta`](firmware/chompi-bootloader-v6.4-beta/) | This bootloader never shipped on units, but was created to improve stability of the Daisy Seed's integration with CHOMPI's hardware as well as repair edge-case issues related to bugs inherited from older versions of the Electrosmith bootloader.  |
| [`firmware/card-profiles`](firmware/card-profiles/) | The factory microSD card contents for TAPE, TEMPO and WAVE — firmware, samples and settings. |
| [`hardware/hardware-pcb`](hardware/hardware-pcb/) | Schematic, BOM, EAGLE PCB files, and the full fabrication package. |
| [`hardware/hardware-enclosure`](hardware/hardware-enclosure/) | The six pcb panel enclosure files, as well as laser cutting files for diy panels. |

Each folder contains its own README, so check those out for more details.

## Card layout — this differs from CHOMPI's instructions

CHOMPI's own docs say to copy a card profile's contents to the **root** of the
card, and the firmwares shipped expecting to find everything there. **This fork
changes that**, so the three firmwares can share one card:

```
/CHOMPI.bin            the launcher -- the ONLY .bin in the root
/FIRMWARE/01_TAPE.bin  the firmwares the launcher offers, one per white key
/FIRMWARE/02_TEMPO.bin
/FIRMWARE/03_WAVE.bin
/TAPE/                 TAPE's samples, options.json, presets.json
/TEMPO/                Chromatic/ Slice/ Buffer/, options.json, presets.json
/WAVE/                 wavetables, options.json, presets.json
```

Nothing but the launcher lives in the root.

**Why.** The root was a shared *and mutable* namespace: TAPE keeps 168 samples
there and writes to it while sampling, WAVE scans it for `.wav` and preloads
the first seven it finds, and all three keep `options.json` / `presets.json`
there in incompatible formats. Put them on one card and WAVE loads TAPE's drum
hits as wavetables and comes up silent, while the settings overwrite each other
on every switch.

**How.** Each firmware calls `f_chdir()` into its own directory immediately
after mounting the card, so every relative path it opens — and writes — lands
there instead.

**Stock cards still work.** Each firmware falls back to the root if its folder
is missing, so these binaries behave exactly like the originals on a
single-firmware card. The change is additive.

**Writing your own firmware?** Do the same and it will never collide with
anything else on the card:

```c
f_chdir("/YOURFIRMWARE");
```

`firmware/chompi-launcher/make-card.sh /Volumes/YOUR_CARD` builds this layout
from the factory profiles in `firmware/card-profiles`.

## Risks, and what actually gets written

Low risk, but not zero risk — here is exactly what happens, so you can judge
for yourself.

**What gets written**

- **QSPI flash** — the bootloader installs the launcher here, the same way it
  installs any firmware you put on a card. It happens once, on the first boot
  after the `.bin` changes (the slow rainbow). The bootloader skips the write
  when what is on the card already matches what is in QSPI.
- **Your SD card** — the launcher writes `/FIRMWARE/launcher_log.txt`, and each
  firmware reads and writes its own settings inside its own folder.

**What never gets written**

- **The STM32's internal flash**, where the bootloader lives. That is the one
  thing that could genuinely brick a CHOMPI, and nothing here touches it. No
  DFU step, no chip programmer, no soldering.

**Switching firmwares does not flash anything.** Picking one copies it into RAM
and resets; QSPI keeps holding the launcher. You write to flash *less* than you
would by swapping cards.

**If something goes wrong**

1. Power cycle — you land back at the picker.
2. Put your old card in — the bootloader installs that firmware and you are
   back on stock.
3. Worst case, <https://flash.daisy.audio> over USB. The Daisy's DFU mode is in
   factory ROM and cannot be overwritten, so the board is always recoverable.

**Caveats worth knowing**

- Unofficial, as-is, no warranty. Not a CHOMPI Club release.
- It is new. It works here and on a handful of other units, but it has not seen
  a lot of hardware yet.
- Your existing cards are untouched — this is a separate card, and nothing
  migrates or modifies your current setup.
- Each firmware falls back to the card root when its folder is missing, so
  stock cards still work. But if you create an empty `/TAPE` folder, TAPE will
  look there and find nothing.

## What's not here

**The panel artwork.** The graphic set and CHOMPI logos have all been removed for copyright purposes. If you choose to create your own hardware, we ask that you name it something else to avoid trademark infringement.

## Support Guidelines

This is a discontinuation open-source release. As such, this repo is intended to be a permanent source for files and documentation, and will likely not be receiving updates in the future. If you wish to customize your own project, we recommend cloning this repo into your own GitHub.

## Community

Even though this version of CHOMPI is now discontinued, the CLUB is expanding. If you want to discuss this project, share your creations, see what other users have made on their CHOMPI, feel free to check out the CHOMPI Open Source channel on the Chase Bliss Discord.

## License

Everything here is **MIT** — see [`LICENSE`](LICENSE). [`THIRD_PARTY.md`](THIRD_PARTY.md) lists
the work this builds on and the notices that come with it. The CHOMPI name, logo and artwork are
not covered by the license — see [`TRADEMARKS.md`](TRADEMARKS.md).

## HAPPY CHOMPIN'

---

