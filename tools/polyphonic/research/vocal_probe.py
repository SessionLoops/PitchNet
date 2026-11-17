# Input-type probe: polyphony from Basic Pitch, pitch cues from SwiftF0.
# Reads "label<TAB>path" lines on stdin, prints one JSON line of features per file.
# NumPy + onnxruntime only. See docs/testing/vocal-detection-research.md.
import sys, wave, numpy as np, onnxruntime as ort, json, os
M=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','..','..','Resources','models')+os.sep
so=ort.SessionOptions(); so.log_severity_level=3
bp=ort.InferenceSession(M+'basic_pitch_nmp.onnx',so,providers=['CPUExecutionProvider'])
sf=ort.InferenceSession(M+'swift_f0.onnx',so,providers=['CPUExecutionProvider'])
def readwav(p):
    w=wave.open(p); n=w.getnframes(); ch=w.getnchannels(); sw=w.getsampwidth(); sr=w.getframerate(); b=w.readframes(n)
    if sw==2: x=np.frombuffer(b,'<i2').astype(np.float32)/32768
    elif sw==3:
        a=np.frombuffer(b,np.uint8).reshape(-1,3).astype(np.int32); v=a[:,0]|(a[:,1]<<8)|(a[:,2]<<16); v=np.where(v>=1<<23,v-(1<<24),v); x=v.astype(np.float32)/(1<<23)
    elif sw==4: x=np.frombuffer(b,'<i4').astype(np.float32)/2**31
    x=x.reshape(-1,ch).mean(1); return x,sr
def resample(x,sr,tr):
    if sr==tr: return x.astype(np.float32)
    n=int(round(len(x)*tr/sr)); X=np.fft.rfft(x); m=n//2+1
    Y=np.zeros(m,complex); k=min(m,len(X)); Y[:k]=X[:k]; return (np.fft.irfft(Y,n)*n/len(x)).astype(np.float32)
def feats(p, maxsec=30):
    x,sr=readwav(p); x=x[:int(maxsec*sr)]
    # ---- Basic Pitch note posteriorgram
    a=resample(x,sr,22050); W=43844
    nw=max(1,int(np.ceil(len(a)/W))); a2=np.zeros(nw*W,np.float32); a2[:len(a)]=a
    notes=bp.run(['StatefulPartitionedCall:1'],{'serving_default_input_2:0':a2.reshape(nw,W,1)})[0].reshape(-1,88)
    notes=notes[:int(len(a)/256)]
    act=notes>0.5
    # count local-maximum active bins (avoid semitone smear)
    pk=act&(notes>=np.roll(notes,1,1))&(notes>=np.roll(notes,-1,1))
    cnt=pk.sum(1); on=cnt>=1
    poly2=float((cnt[on]>=2).mean()) if on.any() else 0.0
    meanpoly=float(cnt[on].mean()) if on.any() else 0.0
    # ---- SwiftF0
    b=resample(x,sr,16000)
    pitch,conf=sf.run(['pitch','confidence'],{'audio':b.reshape(1,-1),'fmin':np.array(46.875,np.float32),'fmax':np.array(2093.75,np.float32)})
    pitch=np.asarray(pitch,np.float64).ravel(); conf=np.asarray(conf).ravel()
    hop=256; nf=len(conf)
    rms=np.array([np.sqrt(np.mean(b[i*hop:i*hop+1024]**2)+1e-12) for i in range(nf)])
    db=20*np.log10(rms+1e-9); loud=db>db.max()-40
    voiced=(conf>0.9)&loud
    vr=float(voiced[loud].mean()) if loud.any() else 0
    # unvoiced-but-loud frames with energy mostly above 3 kHz (sibilance/breath proxy)
    hf=[]
    for i in range(nf):
        seg=b[i*hop:i*hop+1024]
        if len(seg)<1024: hf.append(0); continue
        S=np.abs(np.fft.rfft(seg*np.hanning(1024)))**2; f=np.fft.rfftfreq(1024,1/16000)
        hf.append(S[f>3000].sum()/(S.sum()+1e-12))
    hf=np.array(hf)
    sib=float(((~voiced)&loud&(hf>0.5)).sum()/max(1,loud.sum()))
    # pitch: off-semitone spread and glide fraction within voiced runs
    c=1200*np.log2(np.maximum(pitch,1)/440.0)
    dc=np.abs(np.diff(c)); vv=voiced[1:]&voiced[:-1]
    glide=float(((dc>15)&(dc<300))[vv].mean()) if vv.any() else 0
    dev=((c[voiced]+50)%100)-50
    # tuning-corrected spread: circular mean
    if voiced.any():
        ang=2*np.pi*c[voiced]/100; mu=np.angle(np.mean(np.exp(1j*ang)))*100/(2*np.pi)
        d=((c[voiced]-mu+50)%100)-50; offgrid=float(np.median(np.abs(d)))
    else: offgrid=0
    return dict(dur=len(x)/sr,poly2=poly2,meanpoly=meanpoly,voicedR=vr,sib=sib,glide=glide,offgrid=offgrid)
if __name__=='__main__':
    out=[]
    for line in sys.stdin:
        lab,p=line.strip().split('\t',1)
        try: r=feats(p)
        except Exception as e: print('ERR',p,e,file=sys.stderr); continue
        r['label']=lab; r['file']=os.path.basename(p); out.append(r)
        print(json.dumps(r),flush=True)
