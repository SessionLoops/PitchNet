import warnings; warnings.filterwarnings("ignore")
import sys, math, numpy as np, pandas as pd, soundfile as sf
from scipy.signal import resample_poly
from compare import FCPE, Swift, score, add_noise, reverb_ir, fftconvolve
ROOT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/vocadito'
meta = pd.read_csv(f'{ROOT}/vocadito_metadata.csv').set_index('track_id')
rng = np.random.default_rng(0); ir = reverb_ir(rng)
conds = {'clean': lambda x: x,
         'reverb': lambda x: (lambda y: y/np.abs(y).max()*0.7)(fftconvolve(x, ir)[:len(x)]*0.5 + x*0.5),
         'noise20dB': lambda x: add_noise(x, 20, rng), 'noise10dB': lambda x: add_noise(x, 10, rng),
         'noise0dB': lambda x: add_noise(x, 0, rng)}
models = [FCPE(), Swift()]
for m in models: m(np.random.randn(32000).astype(np.float32)*0.01)
rows = []
for i in range(1, 41):
    x, sr = sf.read(f'{ROOT}/Audio/vocadito_{i}.wav'); x = x.mean(1) if x.ndim > 1 else x
    a = np.loadtxt(f'{ROOT}/Annotations/F0/vocadito_{i}_f0.csv', delimiter=',')
    t = np.arange(len(x))/sr
    gt = a[np.clip(np.searchsorted(a[:,0], t - 0.5*(a[1,0]-a[0,0])), 0, len(a)-1), 1]
    g = math.gcd(16000, int(sr))
    for cn, fn in conds.items():
        y = fn(x); x16 = resample_poly(y, 16000//g, int(sr)//g).astype(np.float32)
        for m in models:
            ts, f0, cf = m(x16)
            r = score(ts*(sr/44100.0), f0, cf, m.th, gt, reverb=(cn=='reverb'))
            r.update(model=m.name, cond=cn, track=i, singer=meta.loc[i,'singer_id'], lang=meta.loc[i,'language']); rows.append(r)
    print('done', i, flush=True)
df = pd.DataFrame(rows); df.to_csv('vocadito_per_track.csv', index=False)
num = ['RPA_pitchonly','RCA_pitchonly','RPA_withvoicing','voicing_recall','false_alarm','voicing_AUC','mae_cents','p95_cents','acc10','octave_err']
def wtd(g): return pd.Series({k: (np.nan if g[k].isna().all() else np.average(g[k].dropna(), weights=g.loc[g[k].notna(),'n_voiced'])) for k in num})
out = '## Vocadito overall (all conditions)\n' + df.groupby('model').apply(wtd).round(4).to_markdown()
out += '\n\n## By condition\n' + df.groupby(['cond','model']).apply(wtd).round(4).to_markdown()
p = df[df.cond=='clean'].pivot(index='track', columns='model', values='RPA_pitchonly')
out += f"\n\n## Clean per-track RPA (pitch only)\nSwiftF0 better on {(p.SwiftF0>p.FCPE+0.005).sum()} tracks, FCPE better on {(p.FCPE>p.SwiftF0+0.005).sum()}, tie {(abs(p.SwiftF0-p.FCPE)<=0.005).sum()}\n"
out += '\n' + p.assign(diff=p.SwiftF0-p.FCPE).sort_values('diff').round(3).iloc[[0,1,2,3,4,-5,-4,-3,-2,-1]].to_markdown()
open('vocadito_results.md','w').write(out); print(out)
