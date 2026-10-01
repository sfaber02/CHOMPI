#!/usr/bin/env bash
#
# Build a multi-firmware CHOMPI card from the factory card profiles.
#
#   ./make-card.sh /Volumes/YOUR_CARD
#
# Lays out one folder per firmware so nothing shares the card root:
#
#   /CHOMPI.bin          the launcher -- the only .bin in the root, so it is
#                        the only thing the stock bootloader ever installs
#   /FIRMWARE/*.bin      the firmwares the launcher offers, one per white key
#   /TAPE/               TAPE's samples and settings
#   /TEMPO/              Chromatic/ Slice/ Buffer/ and settings
#   /WAVE/               wavetables and settings
#
# Each firmware chdir()s into its own folder at startup, and falls back to the
# root if the folder is missing -- so these same binaries still work on a
# stock single-firmware card.
#
# Only adds and overwrites; never deletes anything already on the card.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$(cd "$HERE/.." && pwd)"
PROFILES="$FW/card-profiles"

CARD="${1:-}"
if [[ -z "$CARD" ]]; then
    echo "usage: $(basename "$0") /path/to/card" >&2
    exit 1
fi
if [[ ! -d "$CARD" ]]; then
    echo "error: '$CARD' is not a directory. Is the card mounted?" >&2
    exit 1
fi

# Every binary has to be built first. Point people at the right compiler
# rather than letting them wonder why a file is missing.
need_build=()
for p in chompi-launcher chompi-tape chompi-tempo chompi-wave; do
    [[ -f "$FW/$p/code/src/build/CHOMPI.bin" ]] || need_build+=("$p")
done
if (( ${#need_build[@]} )); then
    echo "error: not built yet: ${need_build[*]}" >&2
    echo "  run 'make' in each code/src. TAPE, WAVE and the launcher need ARM" >&2
    echo "  GCC 10.3-2021.10; TEMPO needs 13.3.rel1." >&2
    exit 1
fi

# A second .bin in the root would race the launcher for the bootloader's
# attention -- it installs the first one it finds.
shopt -s nullglob
stray=("$CARD"/*.bin)
for f in "${stray[@]}"; do
    if [[ "$(basename "$f")" != "CHOMPI.bin" ]]; then
        echo "error: '$f' would compete with the launcher for the bootloader." >&2
        echo "  The root must contain no .bin other than CHOMPI.bin." >&2
        exit 1
    fi
done
shopt -u nullglob

echo "==> launcher"
cp "$FW/chompi-launcher/code/src/build/CHOMPI.bin" "$CARD/CHOMPI.bin"

echo "==> firmwares"
mkdir -p "$CARD/FIRMWARE"
cp "$FW/chompi-tape/code/src/build/CHOMPI.bin"  "$CARD/FIRMWARE/01_TAPE.bin"
cp "$FW/chompi-tempo/code/src/build/CHOMPI.bin" "$CARD/FIRMWARE/02_TEMPO.bin"
cp "$FW/chompi-wave/code/src/build/CHOMPI.bin"  "$CARD/FIRMWARE/03_WAVE.bin"

echo "==> /TAPE"
mkdir -p "$CARD/TAPE"
rsync -a --exclude='*.bin' --exclude='._*' --exclude='.DS_Store' \
      "$PROFILES/tape-2.0/" "$CARD/TAPE/"

echo "==> /TEMPO"
mkdir -p "$CARD/TEMPO"
# Capitalised to match the names in SampleManager.h. FAT is case-insensitive,
# so either spelling works on the hardware; this just keeps them legible.
for d in chromatic:Chromatic slice:Slice buffer:Buffer; do
    rsync -a --exclude='._*' --exclude='.DS_Store' \
          "$PROFILES/tempo-1.0/${d%%:*}/" "$CARD/TEMPO/${d##*:}/"
done
cp "$PROFILES/tempo-1.0/options.json" "$PROFILES/tempo-1.0/presets.json" "$CARD/TEMPO/"

echo "==> /WAVE"
mkdir -p "$CARD/WAVE"
rsync -a --exclude='*.bin' --exclude='._*' --exclude='.DS_Store' \
      "$PROFILES/wave-1.0/" "$CARD/WAVE/"

sync

echo
echo "card ready:"
printf '  /CHOMPI.bin   %s bytes\n' "$(wc -c < "$CARD/CHOMPI.bin" | tr -d ' ')"
printf '  /FIRMWARE     %s firmwares\n' "$(ls "$CARD"/FIRMWARE/*.bin | wc -l | tr -d ' ')"
printf '  /TAPE         %s files\n' "$(ls "$CARD/TAPE" | wc -l | tr -d ' ')"
printf '  /TEMPO        Chromatic=%s Slice=%s Buffer=%s\n' \
    "$(ls "$CARD/TEMPO/Chromatic" | wc -l | tr -d ' ')" \
    "$(ls "$CARD/TEMPO/Slice" | wc -l | tr -d ' ')" \
    "$(ls "$CARD/TEMPO/Buffer" | wc -l | tr -d ' ')"
printf '  /WAVE         %s files\n' "$(ls "$CARD/WAVE" | wc -l | tr -d ' ')"

root_wav=$(ls "$CARD"/*.wav 2>/dev/null | wc -l | tr -d ' ')
root_json=$(ls "$CARD"/*.json 2>/dev/null | wc -l | tr -d ' ')
echo
if [[ "$root_wav" == "0" && "$root_json" == "0" ]]; then
    echo "  root is clean: no .wav or .json outside a firmware folder"
else
    echo "  NOTE: root still holds $root_wav .wav and $root_json .json file(s)."
    echo "  Leftovers from a single-firmware card can be deleted; the firmwares"
    echo "  now read from their own folders."
fi
