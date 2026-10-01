# CHOMPI — Card Profiles

The contents of the factory card profiles for **CHOMPI**: the firmware, the sample set and the
settings files that make up each card.

```
tape-2.0/     TAPE 2.0 — firmware, 168 samples, options and presets
tempo-1.0/    TEMPO 1.0 — firmware, 29 samples in chromatic/, slice/ and buffer/, options and presets
wave-1.0/     WAVE 1.0 — firmware, the seven wavetables, options and 14 presets
```

Each folder is one complete card. Copy its contents to the root of a FAT32 microSD card, put the
card in CHOMPI, and power on — the bootloader installs the firmware from the card.

> **Note for this fork:** the above is how stock CHOMPI works, and the firmwares here still
> support it. But if you want more than one firmware on a card, do not copy these to the root —
> each firmware now reads from its own folder (`/TAPE`, `/TEMPO`, `/WAVE`). Use
> [`../chompi-launcher/make-card.sh`](../chompi-launcher/make-card.sh), which builds that layout
> from these profiles. See the [repository README](../../README.md#card-layout--this-differs-from-chompis-instructions).
