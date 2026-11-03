"""Fetch 28 additional real recordings into a separate recording-holdout workdir.

    python fetch_holdout_samples.py WORK --source-work PRIMARY_WORK
    python make_catalog.py WORK --piano-velocity 8
    python make_corpora.py WORK --partition ocorpus --seed 20261103 --count 80

Reuse the pinned partial clones from fetch_samples.sh; checkout only the named
files into WORK/data, keeping the primary recording catalog unchanged. These
recordings are reserved for final evaluation, not training or model selection.
"""
import argparse
from pathlib import Path
import subprocess


REVISIONS = dict(sal='3382bf9496bba2486f5ab0de55a264d1dfc38404',
                 vsco='440300901dfe9275fd84e0b7763af1f8443ae62e')


def selections():
    sal = [f'Samples/{note}v8.flac' for note in ('C2', 'F#3', 'C5', 'D#6')]
    vsco = [f'Woodwinds/Clarinet/susLong/DCClar_susLong_{note}_v3_rr1_sum.wav' for note in ('D3', 'F4', 'A#4')]
    vsco += [f'Woodwinds/Flute/susNV/LDFlute_susNV_{note}_v{velocity}_1.wav'
             for note, velocity in (('C4', 3), ('A4', 3), ('E5', 2))]
    vsco += [f'Woodwinds/Bassoon/sus/PSBassoon_{note}_v1_1.wav' for note in ('C2', 'C3', 'C4')]
    vsco += [f'Woodwinds/Oboe/Sus/Oboe_Sus_{note}_v1_Main.wav' for note in ('D4', 'A#4', 'F5')]
    vsco += [f'Strings/Violin Section/susVib/VlnEns_susVib_{note}_v2.wav' for note in ('A3', 'D3', 'B4')]
    vsco += [f'Strings/Cello Section/susvib/susvib_{note}_v3_1.wav' for note in ('C3', 'E3', 'D4')]
    # These instruments have no alternate velocity/take for the selected keys.
    vsco += [f'Strings/Harp/KSHarp_{note}_mf.wav' for note in ('F2', 'A2', 'B5')]
    vsco += [f'Keys/Organ/Loud/Rode_Man3Open_{note}.wav' for note in ('22', '43', '46')]
    return dict(sal=sal, vsco=vsco)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('work', type=Path)
    parser.add_argument('--source-work', required=True, type=Path)
    args = parser.parse_args()
    if args.work.resolve() == args.source_work.resolve():
        parser.error('holdout workdir must differ from the primary workdir')
    for name, files in selections().items():
        source = (args.source_work / 'data' / name).resolve()
        target = (args.work / 'data' / name).resolve()
        git_dir = source / '.git'
        revision = REVISIONS[name]
        subprocess.run(['git', '-C', str(source), 'cat-file', '-e', revision + '^{commit}'], check=True)
        target.mkdir(parents=True, exist_ok=True)
        (target / '.git').write_text(f'gitdir: {git_dir}\n')
        subprocess.run(['git', '-c', 'gc.auto=0', f'--git-dir={git_dir}', f'--work-tree={target}',
                        'checkout', revision, '--', 'LICENSE', 'README.md', *files], check=True)
        print(f'{name}: {len(files)} held-out recordings from {revision}', flush=True)


if __name__ == '__main__':
    main()
