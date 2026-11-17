# AudioSet tagger columns for the input-type probe: max score over voice classes (_v) and
# over instrument classes (_i) for YAMNet (y), EfficientAT mn05 (m05) and mn10 (m10).
# Reads "label<TAB>path" lines on stdin; YAMNet from Resources/models, EfficientAT from Resources/tools. See docs/testing/vocal-detection-research.md.
import sys, os, json, numpy as np, onnxruntime as ort
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vocal_probe import readwav, resample
T=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','..','..','Resources','tools')+os.sep
so=ort.SessionOptions(); so.log_severity_level=3
M=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','..','..','Resources','models')+os.sep
S={m:ort.InferenceSession((M if m=='yamnet' else T)+m+'.onnx',so,providers=['CPUExecutionProvider']) for m in ['yamnet','efficientat_mn05_as','efficientat_mn10_as']}
EAT=dict(voice=list(range(0,8))+list(range(27,38)), inst=list(range(138,216)))
YAM=dict(voice=list(range(0,6))+list(range(24,33)), inst=list(range(133,211)))
def eat(sess,x,sr):
    a=resample(x,sr,32000); W=320000
    if len(a)<W: a=np.pad(a,(0,W-len(a)))
    starts=list(range(0,len(a)-W+1,W//2)) or [0]
    sc=np.stack([sess.run(['scores'],{'waveform':a[s:s+W][None]})[0][0] for s in starts])
    return sc.mean(0)
def yam(x,sr):
    a=resample(x,sr,16000); return S['yamnet'].run(['scores'],{'waveform':a})[0].mean(0)
for line in sys.stdin:
    lab,p=line.strip().split('\t',1)
    x,sr=readwav(p); x=x[:30*sr]; out=dict(label=lab,file=os.path.basename(p))
    for name,sc,C in [('y',yam(x,sr),YAM),('m05',eat(S['efficientat_mn05_as'],x,sr),EAT),('m10',eat(S['efficientat_mn10_as'],x,sr),EAT)]:
        out[name+'_v']=float(sc[C['voice']].max()); out[name+'_i']=float(sc[C['inst']].max())
    print(json.dumps(out),flush=True)
