#!/usr/bin/env bash
# Fetch the single-note recordings the polyphonic engine's training and test mixtures are built from
# (partial clones: only the files listed below are downloaded, ~95 MB).
#
#   Salamander Grand Piano V3 - Alexander Holm, CC BY 3.0 (velocity layer 10)
#   VSCO 2 Community Edition  - Versilian Studios / Ivy Audio, CC0 (woodwinds, strings, harp, organ)
#
# usage: fetch_samples.sh WORK_DIR
set -euo pipefail
WORK=${1:?usage: fetch_samples.sh WORK_DIR}
mkdir -p "$WORK/data"
cd "$WORK/data"

if [ ! -d sal ]; then
  git clone --depth 1 --filter=blob:none --no-checkout https://github.com/sfzinstruments/SalamanderGrandPiano sal
fi
(
  cd sal
  files=(README.md LICENSE)
  for o in 1 2 3 4 5 6; do for n in A C 'D#' 'F#'; do files+=("Samples/${n}${o}v10.flac"); done; done
  git -c gc.auto=0 checkout HEAD -- "${files[@]}"
)

if [ ! -d vsco ]; then
  git clone --depth 1 --filter=blob:none --no-checkout https://github.com/sgossner/VSCO-2-CE vsco
fi
(
  cd vsco
  P=(LICENSE README.md)
  for n in D3 F3 'A#3' D4 F4 'A#4'; do P+=("Woodwinds/Clarinet/susLong/DCClar_susLong_${n}_v2_rr1_sum.wav"); done
  for n in C4 E4 A4 C5 E5 A5; do P+=("Woodwinds/Flute/susNV/LDFlute_susNV_${n}_v1_1.wav"); done
  for n in C2 G2 C3 'D#3' 'G#3' C4; do P+=("Woodwinds/Bassoon/sus/PSBassoon_${n}_v2_1.wav"); done
  for n in D4 F4 'A#4' D5 F5; do P+=("Woodwinds/Oboe/Sus/Oboe_Sus_${n}_v3_Main.wav"); done
  for n in A3 C4 D3 E4 G4 'F#3' B4; do P+=("Strings/Violin Section/susVib/VlnEns_susVib_${n}_v1.wav"); done
  for n in C3 E3 G3 D4 B3; do P+=("Strings/Cello Section/susvib/susvib_${n}_v1_1.wav"); done
  for n in C3 E3 G3 C5 E5 G5 D4 F4 A4 B3; do P+=("Strings/Harp/KSHarp_${n}_mf.wav"); done
  for n in 25 28 31 34 37 40; do P+=("Keys/Organ/Loud/Rode_Man3Open_${n}.wav"); done
  git -c gc.auto=0 checkout HEAD -- "${P[@]}"
)
echo "samples in $WORK/data"
