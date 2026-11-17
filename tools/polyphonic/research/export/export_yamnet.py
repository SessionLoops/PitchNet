# Convert the official YAMNet (tensorflow/models research/audioset/yamnet: features.py, params.py,
# yamnet.h5) to ONNX. The network is the Keras definition in yamnet.py rewritten as plain TF ops
# (tf_keras does not load under TF 2.22); features.py is used unchanged.
# Input "waveform" float32 [samples] mono 16 kHz in [-1,1]. Outputs: "scores" [patches,521],
# "embeddings" [patches,1024], "log_mel" [frames,64]. One patch = 0.96 s, hop 0.48 s.
import sys, csv, h5py, numpy as np, tensorflow as tf, tf2onnx, onnxruntime as ort
src, out = sys.argv[1:3]; sys.path.insert(0, src)
import params as P, features as F
h5 = h5py.File(f'{src}/yamnet.h5', 'r')
def W(n): return tf.constant(np.array(h5[n]))
DEFS = [('c',2,32),('s',1,64),('s',2,128),('s',1,128),('s',2,256),('s',1,256),('s',2,512),
        ('s',1,512),('s',1,512),('s',1,512),('s',1,512),('s',1,512),('s',2,1024),('s',1,1024)]
EPS = P.Params().batchnorm_epsilon
def bn(x, p):  # center=True, scale=False
    m, v, b = W(f'{p}/{p}/moving_mean:0'), W(f'{p}/{p}/moving_variance:0'), W(f'{p}/{p}/beta:0')
    return (x - m) * tf.math.rsqrt(v + EPS) + b
def net(patches):
    x = patches[..., None]
    for i, (t, s, _) in enumerate(DEFS, 1):
        L = f'layer{i}'
        if t == 'c':
            x = tf.nn.relu(bn(tf.nn.conv2d(x, W(f'{L}/conv/{L}/conv/kernel:0'), s, 'SAME'), f'{L}/conv/bn'))
        else:
            x = tf.nn.relu(bn(tf.nn.depthwise_conv2d(x, W(f'{L}/depthwise_conv/{L}/depthwise_conv/depthwise_kernel:0'), [1,s,s,1], 'SAME'), f'{L}/depthwise_conv/bn'))
            x = tf.nn.relu(bn(tf.nn.conv2d(x, W(f'{L}/pointwise_conv/{L}/pointwise_conv/kernel:0'), 1, 'SAME'), f'{L}/pointwise_conv/bn'))
    e = tf.reduce_mean(x, [1, 2])
    return tf.sigmoid(e @ W('logits/logits/kernel:0') + W('logits/logits/bias:0')), e
def model(w, tfl):
    p = P.Params(tflite_compatible=tfl)
    lm, patches = F.waveform_to_log_mel_spectrogram_patches(F.pad_waveform(w, p), p)
    s, e = net(patches); return s, e, lm
spec = (tf.TensorSpec([None], tf.float32, name='waveform'),)
@tf.function(input_signature=spec)
def f(w):
    s, e, l = model(w, True)
    return {'scores': tf.identity(s, 'scores'), 'embeddings': tf.identity(e, 'embeddings'), 'log_mel': tf.identity(l, 'log_mel')}
tf2onnx.convert.from_function(f, input_signature=spec, opset=17, output_path=out)
so = ort.InferenceSession(out, providers=['CPUExecutionProvider'])
print([(o.name, o.shape) for o in so.get_outputs()])
rng = np.random.default_rng(0)
for n in (8000, 15600, 16000*3, 16000*30+123):
    w = (rng.standard_normal(n) * 0.1).astype(np.float32)
    r = model(tf.constant(w), False)[0].numpy(); o = so.run(['scores'], {'waveform': w})[0]
    print(f'{n} samples: patches {o.shape[0]}, max |onnx - tf(stft)| = {np.abs(o - r).max():.2e}')
names = [r[2] for r in list(csv.reader(open(f'{src}/yamnet_class_map.csv')))[1:]]
t = np.arange(16000*3)/16000
for lab, w in [('1 kHz sine', 0.5*np.sin(2*np.pi*1000*t)), ('white noise', 0.3*rng.standard_normal(len(t)))]:
    s = so.run(['scores'], {'waveform': w.astype(np.float32)})[0].mean(0); k = np.argsort(s)[::-1][:3]
    print(lab, '->', [(names[i], round(float(s[i]), 3)) for i in k])
