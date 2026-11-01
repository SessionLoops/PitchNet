# F0 detector comparison: FCPE vs SwiftF0

Offline evaluation behind switching PitchNet's default pitch detector from
FCPE to SwiftF0. `compare.py` reimplements FCPE exactly as
`Source/Audio/FCPEPitchDetector.cpp` runs it (same mel front end and
decoder), so the numbers reflect what PitchNet actually did.

Requirements: `pip install numpy scipy pandas soundfile onnxruntime swift-f0`

## Synthetic vocals (exact ground truth)

`synth.py` generates sung vowels (source-filter model with vibrato,
portamento, jitter, shimmer and breath noise) for bass to soprano.

    python compare.py ../../Resources/models          # SEED=7 by default
    SEED=21 python compare.py ../../Resources/models

Results: `results.md`, `results_seed*.md`, `results_per_clip*.csv`.

## Real vocals: vocadito

40 solo vocal tracks with F0 annotations. Not committed (58.5 MB, CC BY 4.0):
download `vocadito.zip` from https://zenodo.org/records/5557945 (v3) and unzip
it, then

    python vocadito_eval.py /path/to/vocadito

Results: `vocadito_results.md`, `vocadito_per_track.csv`.

> Bittner, R. M., Pasalo, K., Bosch, J. J., Meseguer-Brocal, G., & Rubinstein, D.
> (2021). vocadito: A dataset of solo vocals with f0, note, and lyric annotations.

Each script also runs under five conditions: clean, reverb, and noise at
20, 10 and 0 dB SNR.
