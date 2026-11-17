# Export EfficientAT mn05_as / mn10_as to ONNX with the mel frontend baked in.
# Input: "waveform" float32 [batch, samples] mono at 32 kHz.
# Outputs: "scores" [batch, 527] sigmoid AudioSet scores; "embedding" [batch, C].
import sys, torch, torch.nn as nn, numpy as np, onnxruntime as ort
repo, wdir, outdir = sys.argv[1:4]
sys.path.insert(0, repo)
from models.mn.model import get_model
from models.preprocess import AugmentMelSTFT
import torchaudio

class Frontend(nn.Module):
    # Same maths as AugmentMelSTFT in eval mode, with STFT as a conv1d (ONNX-friendly).
    def __init__(s, n_mels=128, sr=32000, win=800, hop=320, n_fft=1024):
        super().__init__(); s.hop=hop; s.n_fft=n_fft
        w=torch.hann_window(win, periodic=False); lp=(n_fft-win)//2
        w=torch.nn.functional.pad(w,(lp,n_fft-win-lp))
        k=torch.arange(n_fft//2+1).float()[:,None]; n=torch.arange(n_fft).float()[None,:]
        ang=2*np.pi*k*n/n_fft
        s.register_buffer('kr',(torch.cos(ang)*w)[:,None,:]); s.register_buffer('ki',(-torch.sin(ang)*w)[:,None,:])
        mb,_=torchaudio.compliance.kaldi.get_mel_banks(n_mels,n_fft,sr,0.0,sr//2-1000,vtln_low=100.0,vtln_high=-500.,vtln_warp_factor=1.0)
        s.register_buffer('mel',torch.nn.functional.pad(mb,(0,1)))
    def forward(s,x):
        x=x[:,1:]-0.97*x[:,:-1]                      # pre-emphasis (conv1d [-.97,1], valid)
        x=torch.nn.functional.pad(x[:,None,:],(s.n_fft//2,s.n_fft//2),mode='reflect')
        p=torch.nn.functional.conv1d(x,s.kr,stride=s.hop)**2+torch.nn.functional.conv1d(x,s.ki,stride=s.hop)**2
        m=torch.matmul(s.mel,p)
        return ((m+1e-5).log()+4.5)/5.

class Tagger(nn.Module):
    def __init__(s,m): super().__init__(); s.f=Frontend(); s.m=m
    def forward(s,w):
        logits,emb=s.m(s.f(w).unsqueeze(1)); return torch.sigmoid(logits),emb

for name,width,f in [('mn05_as',0.5,'mn05_as_mAP_443.pt'),('mn10_as',1.0,'mn10_as_mAP_471.pt')]:
    m=get_model(width_mult=width,pretrained_name=None); m.load_state_dict(torch.load(f'{wdir}/{f}',map_location='cpu')); m.eval()
    t=Tagger(m).eval()
    torch.manual_seed(0); w=torch.randn(1,32000*10)*0.1
    with torch.no_grad():
        ref=AugmentMelSTFT(n_mels=128,sr=32000,win_length=800,hopsize=320).eval()
        sp=ref(w); r=torch.sigmoid(m(sp.unsqueeze(0))[0]); mine=t.f(w)
        print(name,'mel max diff',float((sp-mine).abs().max()))
    out=f'{outdir}/efficientat_{name}.onnx'
    torch.onnx.export(t,(w,),out,input_names=['waveform'],output_names=['scores','embedding'],
        dynamic_axes={'waveform':{0:'batch',1:'samples'},'scores':{0:'batch'},'embedding':{0:'batch'}},opset_version=17,dynamo=False)
    so=ort.InferenceSession(out,providers=['CPUExecutionProvider'])
    for secs in (10,3,30):
        torch.manual_seed(secs); w=torch.randn(1,32000*secs)*0.1
        with torch.no_grad(): r=torch.sigmoid(m(ref(w).unsqueeze(0))[0]).numpy()
        o=so.run(['scores'],{'waveform':w.numpy()})[0]
        print(f'  {secs}s ORT vs torch(original frontend) max diff {np.abs(o-r).max():.2e}')
