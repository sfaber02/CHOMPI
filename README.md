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

