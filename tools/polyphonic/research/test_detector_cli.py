"""Check CLI detector defaults, overrides, fallback and JSON provenance.

Run explicitly with the two executables built from the same source tree:

    python test_detector_cli.py --harmonic-cli BUILD/polyphonic_cli \
        --guided-cli BUILD/polyphonic_guided_cli [--model basic_pitch_nmp.onnx]

The default fixture is the local recorded piano triad. --audio can select any
available WAV. These checks compare equivalent invocations; they do not freeze
model predictions or assert transcription accuracy. Only the standard library
is required. They are not skipped tests in unittest discovery.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def check_detectors(harmonic_cli, guided_cli, audio, model=None):
    assertions = 0

    def require(condition, message):
        nonlocal assertions
        assertions += 1
        if not condition:
            raise AssertionError(message)

    with tempfile.TemporaryDirectory(prefix='pitchnet-detector-cli-') as folder:
        root = Path(folder)

        def run(cli, name, expected_mode, *options):
            output = root / (name + '.json')
            result = subprocess.run([str(cli), str(audio), '--json', str(output), *map(str, options)],
                                    capture_output=True, text=True, check=True)
            document = json.loads(output.read_text())
            require(document.get('detector') == expected_mode,
                    f'{name}: JSON does not identify {expected_mode} mode')
            require(isinstance(document.get('notes'), list), f'{name}: missing note list')
            # The strategy label differs for fallback runs, but analysis must be identical.
            analysis = {key: value for key, value in document.items() if key != 'detector'}
            return analysis, result

        baseline, _ = run(harmonic_cli, 'harmonic-default', 'harmonic')
        for cli, name, expected_mode, options in (
                (harmonic_cli, 'harmonic-explicit', 'harmonic', ['--detector', 'harmonic']),
                (harmonic_cli, 'harmonic-to-guided-fallback', 'guided', ['--detector', 'guided']),
                (guided_cli, 'guided-default-fallback', 'guided', []),
                (guided_cli, 'guided-to-harmonic', 'harmonic', ['--detector', 'harmonic'])):
            analysis, _ = run(cli, name, expected_mode, *options)
            require(analysis == baseline, f'{name}: no-evidence fallback changed harmonic analysis')

        for cli in (harmonic_cli, guided_cli):
            for options in (['--detector', 'unsupported'], ['--detector']):
                result = subprocess.run([str(cli), str(audio), *options],
                                        capture_output=True, text=True)
                require(result.returncode == 2, f'{cli.name}: invalid detector was not rejected')
                require('--detector expects harmonic or guided' in result.stderr,
                        f'{cli.name}: invalid detector did not explain accepted values')

        if model is not None:
            evidence_file = root / 'recorded-note-evidence.ev'
            harmonic, _ = run(harmonic_cli, 'harmonic-model', 'harmonic',
                              '--model', model, '--dump-evidence', evidence_file)
            guided, _ = run(guided_cli, 'guided-model', 'guided', '--model', model)
            overridden, _ = run(harmonic_cli, 'harmonic-to-guided-model', 'guided',
                                '--detector', 'guided', '--model', model)
            require(overridden == guided, 'guided override differs from guided executable default')
            overridden, _ = run(guided_cli, 'guided-to-harmonic-model', 'harmonic',
                                '--detector', 'harmonic', '--model', model)
            require(overridden == harmonic, 'harmonic override differs from harmonic executable default')
            cached, _ = run(guided_cli, 'guided-cached-evidence', 'guided', '--evidence', evidence_file)
            require(cached == guided, 'legacy note-only evidence differs from computed evidence')

    return assertions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harmonic-cli', required=True, type=Path)
    parser.add_argument('--guided-cli', required=True, type=Path)
    parser.add_argument('--audio', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'test-audio/01-piano-triad-C-major.wav')
    parser.add_argument('--model', type=Path)
    args = parser.parse_args()
    for name in ('harmonic_cli', 'guided_cli', 'audio', 'model'):
        path = getattr(args, name)
        if path is not None:
            path = path.resolve()
            if not path.is_file():
                parser.error(f'{name.replace("_", "-")} file not found: {path}')
            setattr(args, name, path)
    assertions = check_detectors(args.harmonic_cli, args.guided_cli, args.audio, args.model)
    print(f'CLI detector checks passed: {assertions} assertions '
          f'({"with model and cached evidence" if args.model else "without neural evidence"})')


if __name__ == '__main__':
    main()
