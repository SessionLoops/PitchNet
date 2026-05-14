<div align="center">
  <h1>PitchNet</h1>
  <img src="logo.png" alt="Logo" width="300" />
  <p>A lightweight pitch editor built with JUCE, featuring neural pitch detection and vocoder resynthesis for real-time preview.</p>
</div>

## Overview

PitchNet is a standalone app and audio plugin for editing vocal pitch in a piano roll. It analyzes audio using neural pitch detectors (RMVPE / FCPE), segments notes with the GAME model, and resynthesizes edits in real time through a neural vocoder (PC-NSF-HiFiGAN). Edits are previewed instantly with minimal latency and exported as audio or MIDI. The plugin supports both ARA hosts and a non-ARA capture workflow.

## Features

- Piano roll editing with select, draw, split, and stretch modes
- Note timing stretch with absorb (zero-sum) and ripple (shift subsequent) modes
- Neural pitch detection (RMVPE default, FCPE optional)
- GAME-based note segmentation with D3PM diffusion model
- Pitch curve editing with per-note delta, scale, offset, tilt, and variance controls
- Real-time incremental vocoder resynthesis (PC-NSF-HiFiGAN)
- Undo/redo for all editing operations
- Export audio (WAV) and MIDI
- Standalone app plus VST3/AU plugins (AAX optional, ARA supported)
- GPU acceleration via CUDA or DirectML on Windows; CoreML on macOS
- Multi-language UI (English, Japanese, Chinese Simplified, Chinese Traditional)

## Download
To [Release](https://github.com/KCKT0112/PitchNet/releases)

## Requirements

- CMake 3.22+
- C++17 compiler
- Git (for submodules)

### Platform notes

| Platform | Requirements |
| --- | --- |
| Windows | Visual Studio 2019+ with C++ workload, Windows SDK |
| macOS | Xcode + command line tools |
| Linux | GCC/Clang, ALSA development libraries |

## Models and Assets

The build expects required model files in `Resources/models` and will fail if any are missing:

- `pc_nsf_hifigan.onnx` (vocoder)
- `rmvpe.onnx` (pitch detection)
- `fcpe.onnx` (pitch detection, optional)
- `GAME/` (note segmentation: `encoder.onnx`, `estimator.onnx`, `segmenter.onnx`, `dur2bd.onnx`, `bd2dur.onnx`, `config.json`)
- `cent_table.bin`
- `mel_filterbank.bin`

Other runtime assets live in `Resources/lang` and `Resources/fonts`.

## Build

### Submodules

```bash
git submodule update --init --recursive
```

### Configure

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

### Build

```bash
cmake --build build --config Release
```

### Optional flags

- `-DUSE_CUDA=ON` Enable CUDA execution provider (Windows)
- `-DUSE_DIRECTML=ON` Enable DirectML execution provider (Windows)
- `-DUSE_ASIO=ON` Enable ASIO support (Windows)
- `-DUSE_BUNDLED_CUDA_RUNTIME=ON` Bundle CUDA redistributables (Windows)
- `-DUSE_BUNDLED_DIRECTML_RUNTIME=ON` Bundle DirectML runtime (Windows)
- `-DONNXRUNTIME_VERSION=1.24.3` Override ONNX Runtime version

Notes:
- CUDA and DirectML cannot be enabled at the same time.
- ONNX Runtime is downloaded during CMake configure.

## Plugin Formats

- VST3 and AU are built by default.
- AAX is built only when `AAX_SDK_PATH` is set and valid.
- ARA is enabled when the `third_party/ARA_SDK` submodule is present.

## Usage

1. Open an audio file (WAV/MP3/FLAC/OGG).
2. Analyze to detect pitch and segment notes.
3. Edit notes in the piano roll — adjust pitch, timing, and expression.
4. Use stretch mode to fine-tune note boundaries (absorb or ripple).
5. Preview changes in real time via the neural vocoder.
6. Export the edited audio or MIDI.

### Plugin modes

- **ARA mode**: Direct audio access in supported hosts (e.g., Studio One, Cubase, Logic).
- **Non-ARA mode**: Auto-capture and process in hosts without ARA.

## Project Structure

```
PitchNet/
  Source/
    Audio/        # Audio engine, pitch detection, synthesis, vocoder
    Models/       # Project, note, and audio data models
    UI/           # Piano roll, toolbar, and UI components
    Utils/        # Resampling, localization, mel spectrogram utilities
    Undo/         # Undo/redo action system
    Plugin/       # VST3/AU/AAX/ARA plugin integration
  Resources/
    models/       # ONNX models and data files
    lang/         # Localization JSON (en, ja, zh, zh-TW)
    fonts/        # UI fonts
    svg/          # Toolbar icons
  third_party/
    JUCE/         # JUCE framework (submodule)
    ARA_SDK/      # ARA SDK (submodule, optional)
  CMakeLists.txt
```

## Acknowledgements

This project uses the following models from the open-source community:

- [GAME](https://github.com/openvpi/GAME) — Note segmentation model by [OpenVPI](https://github.com/openvpi)
- [PC-NSF-HiFiGAN](https://github.com/openvpi/vocoders) — Neural vocoder by [OpenVPI](https://github.com/openvpi)
- [RMVPE](https://github.com/yxlllc/RMVPE) — Robust pitch estimation model by [yxlllc](https://github.com/yxlllc)

## License

See `LICENSE`.
