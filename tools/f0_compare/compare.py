import warnings; warnings.filterwarnings("ignore")
import sys, time, json, numpy as np, onnxruntime as ort
from scipy.signal import resample_poly
from synth import make_set, add_noise, reverb_ir, fftconvolve, SR
import swift_f0
MODELS = sys.argv[1] if len(sys.argv) > 1 else '../../Resources/models'

# ---------- FCPE: same front end / decoder as Source/Audio/FCPEPitchDetector.cpp ----------
class FCPE:
    def __init__(self, threshold=0.05, threads=4):  # threshold used only for voicing decision
        so = ort.SessionOptions(); so.intra_op_num_threads = threads
        self.s = ort.InferenceSession(f'{MODELS}/fcpe.onnx', so, providers=['CPUExecutionProvider'])
        self.mel = np.fromfile(f'{MODELS}/mel_filterbank.bin', np.float32).reshape(128, 513)
        self.cent = np.fromfile(f'{MODELS}/cent_table.bin', np.float32)
        self.win = np.hanning(1025)[:-1].astype(np.float32)  # periodic hann
        self.th = threshold; self.name = 'FCPE'
    def __call__(self, x16):
        W, H = 1024, 160; pl = (W - H)//2; pr = max((W - H + 1)//2, W - len(x16) - pl)
        xp = np.pad(x16, (pl, pr), mode='reflect').astype(np.float32)
        n = 1 + (len(xp) - W)//H
        idx = np.arange(W)[None] + H*np.arange(n)[:, None]
        mag = np.sqrt(np.abs(np.fft.rfft(xp[idx]*self.win, axis=1))**2 + 1e-9)
        mel = np.log(np.maximum(mag @ self.mel.T, 1e-5)).astype(np.float32)[None]
        lat = self.s.run(None, {self.s.get_inputs()[0].name: mel})[0][0]
        f0 = np.zeros(len(lat)); conf = lat.max(1)
        for t in range(len(lat)):
            mi = lat[t].argmax()
            a, b = max(0, mi-4), min(359, mi+4)
            c = (self.cent[a:b+1]*lat[t, a:b+1]).sum()/lat[t, a:b+1].sum()
            f0[t] = 10*2**(c/1200)
        return np.arange(len(lat))*H/16000, f0, conf

class Swift:
    def __init__(self, thr=0.5, threads=4):
        self.m = swift_f0.SwiftF0(threads=threads); self.name = 'SwiftF0'; self.th = thr
    def __call__(self, x16):
        r = self.m.detect(x16, 16000)
        return r.timestamps, r.pitch_hz.copy(), r.confidence

# ---------- metrics ----------
def cents(a, b): return 1200*np.log2(a/b)
def auc(pos, neg):
    from scipy.stats import rankdata
    if len(pos) == 0 or len(neg) == 0: return np.nan
    r = rankdata(np.concatenate([pos, neg])); return (r[:len(pos)].sum() - len(pos)*(len(pos)+1)/2)/(len(pos)*len(neg))
def score(ts, est, conf, th, gt_f0, tol=50, reverb=False):
    idx = np.clip((ts*SR).astype(int), 0, len(gt_f0)-1)
    gt = gt_f0[idx]; gv = gt > 0
    # frames within 60 ms of a voicing transition (or, for reverb, in unvoiced gaps) are ambiguous for voicing stats
    v = (gt_f0 > 0).astype(int); edges = np.flatnonzero(np.diff(v) != 0)
    near = np.zeros(len(ts), bool)
    if len(edges): near = np.min(np.abs(idx[:, None] - edges[None]), axis=1) < 0.06*SR
    ev = (conf >= th) if th is not None else np.ones(len(ts), bool)
    ok = gv & (est > 0)
    err = np.full(len(ts), np.nan); err[ok] = cents(est[ok], gt[ok])
    # (1) pitch-only accuracy: voicing decision ignored (model's raw pitch on every truly voiced frame)
    rpa_raw = np.mean(np.abs(err[ok]) <= tol)
    rca_raw = np.mean(np.abs(((err[ok]+600) % 1200) - 600) <= tol)
    # (2) with model's own voicing threshold
    both = ok & ev
    rpa = np.sum(np.abs(err[both]) <= tol)/gv.sum()
    good = both & (np.abs(np.nan_to_num(err, nan=1e9)) <= tol); fine = err[good]
    # voicing stats away from boundaries
    far = ~near; vr = (ev & gv & far).sum()/max((gv & far).sum(), 1)
    fa = np.nan if reverb else (ev & ~gv & far).sum()/max((~gv & far).sum(), 1)
    return dict(RPA_pitchonly=rpa_raw, RCA_pitchonly=rca_raw, RPA_withvoicing=rpa,
                voicing_recall=vr, false_alarm=fa,
                voicing_AUC=np.nan if reverb else auc(conf[gv & far], conf[~gv & far]),
                mae_cents=np.mean(np.abs(err[ok & (np.abs(np.nan_to_num(err,nan=1e9))<=tol)])),
                p95_cents=np.percentile(np.abs(fine), 95), acc10=np.mean(np.abs(err[ok]) <= 10),
                octave_err=np.mean(np.abs(np.abs(err[ok]) - 1200) < tol), n_voiced=int(gv.sum()))

def run():
    items, rng = make_set(seed=int(__import__('os').environ.get('SEED',7)))
    ir = reverb_ir(rng)
    conds = {'clean': lambda x: x,
             'reverb': lambda x: (lambda y: y/np.abs(y).max()*0.7)(fftconvolve(x, ir)[:len(x)]*0.5 + x*0.5),
             'noise20dB': lambda x: add_noise(x, 20, rng),
             'noise10dB': lambda x: add_noise(x, 10, rng),
             'noise0dB':  lambda x: add_noise(x, 0, rng)}
    models = [FCPE(), Swift()]
    # warm up
    w = np.random.randn(16000*2).astype(np.float32)*0.01
    for m in models: m(w)
    rows = []; times = {m.name: [0.0, 0.0] for m in models}
    for cname, fn in conds.items():
        for it in items:
            x = fn(it['audio']); x16 = resample_poly(x, 160, 441).astype(np.float32)
            for m in models:
                t0 = time.perf_counter(); ts, f0, cf = m(x16); dt = time.perf_counter()-t0
                times[m.name][0] += dt; times[m.name][1] += len(x16)/16000
                r = score(ts, f0, cf, m.th, it['f0'], reverb=(cname=='reverb')); r.update(model=m.name, cond=cname, clip=it['name']); rows.append(r)
    import pandas as pd
    df = pd.DataFrame(rows); df.to_csv(f"results_per_clip_seed{__import__('os').environ.get('SEED',7)}.csv", index=False)
    num = ['RPA_pitchonly','RCA_pitchonly','RPA_withvoicing','voicing_recall','false_alarm','voicing_AUC','mae_cents','p95_cents','acc10','octave_err']
    wtd = lambda g: pd.Series({k: (np.nan if g[k].isna().all() else np.average(g[k].dropna(), weights=g.loc[g[k].notna(),'n_voiced'])) for k in num})
    by_cond = df.groupby(['cond','model']).apply(wtd).round(4)
    by_voice = df.assign(voice=df['clip'].str.replace(r'\d$','',regex=True)).groupby(['voice','model']).apply(wtd).round(4)
    overall = df.groupby('model').apply(wtd).round(4)
    rtf = {k: v[0]/v[1] for k, v in times.items()}
    with open(f"results_seed{__import__('os').environ.get('SEED',7)}.md",'w') as f:
        f.write('## Overall\n'+overall.to_markdown()+'\n\n## By condition\n'+by_cond.to_markdown()+'\n\n## By voice type\n'+by_voice.to_markdown()+'\n\n## CPU real-time factor (4 threads)\n'+json.dumps(rtf)+'\n')
    print(open(f"results_seed{__import__('os').environ.get('SEED',7)}.md").read())
if __name__ == '__main__': run()
