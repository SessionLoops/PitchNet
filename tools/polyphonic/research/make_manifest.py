"""Record source revisions, recording hashes and generated-corpus hashes.

    python make_manifest.py --work WORK [--out WORK/manifest.json]

The standard training and held-out corpora use the same source recordings. Their
random seeds separate mixtures and voicings; they do not hold out recordings.
Keep this manifest with evaluation results so that this distinction and the
exact source audio remain reviewable.
"""
import argparse
import hashlib
from importlib import metadata
import json
from pathlib import Path
import platform
import subprocess


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--recording-holdout-of', type=Path,
                        help='verify that no source-recording hash occurs in this reference catalog')
    args = parser.parse_args()
    work = args.work.resolve()
    repo = Path(__file__).resolve().parents[3]
    catalog = json.loads((work / 'samples/catalog.json').read_text())
    sources = {}
    for name in ('sal', 'vsco'):
        root = work / 'data' / name
        sources[name] = dict(url=git(root, 'remote', 'get-url', 'origin'), commit=git(root, 'rev-parse', 'HEAD'),
                             recordings={str(path.relative_to(root)): sha256(path)
                                         for path in sorted(root.rglob('*'))
                                         if path.is_file() and '.git' not in path.parts
                                         and path.suffix.lower() in ('.wav', '.flac')})
    corpora = {}
    for name, seed, purpose in (('corpus', None, 'hand-made diagnostic examples'),
                               ('rcorpus', 12345, 'training mixtures'),
                               ('rcorpus_next', 20261102, 'additional training mixtures'),
                               ('vcorpus', 20261026, 'validation mixtures, shared source recordings'),
                               ('tcorpus', 999, 'held-out mixtures, shared source recordings'),
                               ('pcorpus', 4242, 'repeated-note mixtures, shared source recordings'),
                               ('ocorpus', 20261103, 'final evaluation on held-out source recordings')):
        root = work / name
        if not root.is_dir():
            continue
        files = sorted(path for path in root.glob('*') if path.suffix in ('.npz', '.json'))
        corpora[name] = dict(seed=seed, purpose=purpose, mixtures=len(list(root.glob('*.npz'))),
                             files={path.name: sha256(path) for path in files})
    generated_samples = {entry['path']: sha256(work / entry['path']) for entry in catalog}
    source_files = list((repo / 'Source/Audio/Polyphonic').glob('*.[ch]*'))
    source_files += list(Path(__file__).resolve().parent.glob('*.py'))
    source_files += [Path(__file__).resolve().parent / 'fetch_samples.sh', repo / 'Resources/models/basic_pitch_nmp.onnx']
    packages = {}
    for name in ('numpy', 'scipy', 'soundfile', 'scikit-learn'):
        try:
            packages[name] = metadata.version(name)
        except metadata.PackageNotFoundError:
            packages[name] = None
    recording_independence = False
    reference = None
    if args.recording_holdout_of:
        reference_path = args.recording_holdout_of / 'samples/catalog.json'
        reference_catalog = json.loads(reference_path.read_text())
        if any('source_sha256' not in entry for entry in catalog + reference_catalog):
            parser.error('catalogs need source_sha256 fields; rebuild them with make_catalog.py')
        overlap = set(entry['source_sha256'] for entry in catalog) & set(entry['source_sha256'] for entry in reference_catalog)
        if overlap:
            parser.error(f'recording holdout shares {len(overlap)} source recording hashes with the reference catalog')
        recording_independence = True
        reference = dict(catalog_sha256=sha256(reference_path), source_recording_overlap=0)
    manifest = dict(version=1, repository_commit=git(repo, 'rev-parse', 'HEAD'), python=platform.python_version(),
                    packages=packages, sources=sources, catalog_sha256=sha256(work / 'samples/catalog.json'),
                    sample_count=len(catalog), generated_samples=generated_samples, corpora=corpora,
                    recording_independence=recording_independence, reference_catalog=reference,
                    split_description=('No source recording hash occurs in the reference catalog; use only for final evaluation.'
                                       if recording_independence else
                                       'rcorpus, rcorpus_next, vcorpus and tcorpus have different seeds/voicings but share source recordings.'),
                    implementation={str(path.relative_to(repo)): sha256(path) for path in sorted(source_files)})
    output = args.out or work / 'manifest.json'
    output.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'{len(catalog)} source recordings; ' + ', '.join(f'{name}: {value["mixtures"]}' for name, value in corpora.items()))
    print(f'Manifest: {output.resolve()}')


if __name__ == '__main__':
    main()
