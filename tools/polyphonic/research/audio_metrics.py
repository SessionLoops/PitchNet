"""Small NumPy-only WAV and spectral-score helpers for local regression examples."""
from pathlib import Path
import struct

import numpy as np


def read_wav(path):
    """Return (samples x channels float64 audio, sample rate) from a RIFF WAV."""
    raw = Path(path).read_bytes()
    if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
        raise ValueError(f'{path}: expected a little-endian RIFF WAV')
    fmt = data = None
    offset = 12
    while offset + 8 <= len(raw):
        tag, size = struct.unpack_from('<4sI', raw, offset)
        chunk = raw[offset + 8:offset + 8 + size]
        if len(chunk) != size:
            raise ValueError(f'{path}: truncated WAV chunk')
        if tag == b'fmt ':
            fmt = chunk
        elif tag == b'data':
            data = chunk
        offset += 8 + size + size % 2
    if fmt is None or len(fmt) < 16 or data is None:
        raise ValueError(f'{path}: missing WAV format or audio data')
    code, channels, rate, _, _, bits = struct.unpack_from('<HHIIHH', fmt)
    if code == 0xfffe and len(fmt) >= 40:  # WAVE_FORMAT_EXTENSIBLE
        code = struct.unpack_from('<H', fmt, 24)[0]
    if code == 3 and bits in (32, 64):
        audio = np.frombuffer(data, dtype=f'<f{bits // 8}').astype(np.float64)
    elif code == 1 and bits == 8:
        audio = (np.frombuffer(data, dtype=np.uint8).astype(np.float64) - 128) / 128
    elif code == 1 and bits in (16, 32):
        audio = np.frombuffer(data, dtype=f'<i{bits // 8}').astype(np.float64) / 2 ** (bits - 1)
    elif code == 1 and bits == 24:
        packed = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        values = packed[:, 0] | (packed[:, 1] << 8) | (packed[:, 2] << 16)
        audio = ((values ^ 0x800000) - 0x800000).astype(np.float64) / 2 ** 23
    else:
        raise ValueError(f'{path}: unsupported WAV format {code}, {bits} bits')
    if channels < 1 or rate < 1 or len(audio) % channels:
        raise ValueError(f'{path}: invalid channel count, sample rate or data length')
    return audio.reshape(-1, channels), rate


def write_float_wav(path, audio, sample_rate):
    """Write mono or multichannel audio as an IEEE float32 WAV without clipping."""
    audio = np.asarray(audio, dtype='<f4')
    if audio.ndim == 1:
        audio = audio[:, None]
    if audio.ndim != 2 or audio.shape[1] < 1 or sample_rate < 1:
        raise ValueError('expected samples x channels audio and a positive sample rate')
    channels = audio.shape[1]
    payload = audio.tobytes()
    fmt = struct.pack('<HHIIHH', 3, channels, sample_rate, sample_rate * channels * 4, channels * 4, 32)
    Path(path).write_bytes(b'RIFF' + struct.pack('<I', 36 + len(payload)) + b'WAVEfmt '
                           + struct.pack('<I', 16) + fmt + b'data'
                           + struct.pack('<I', len(payload)) + payload)


def magsnr(reference, estimate, n=2048, hop=512):
    """Magnitude spectral SNR in dB; each multichannel input is averaged to mono."""
    reference, estimate = np.asarray(reference), np.asarray(estimate)
    if reference.shape != estimate.shape:
        raise ValueError('reference and estimate must have the same shape')

    def spectrum(audio):
        if audio.ndim == 2:
            audio = audio.mean(axis=1)
        window = np.hanning(n + 1)[:-1]
        frames = np.lib.stride_tricks.sliding_window_view(np.pad(audio, (n // 2, n // 2)), n)[::hop]
        return np.abs(np.fft.rfft(frames * window, axis=1))

    ref, est = spectrum(reference), spectrum(estimate)
    return float(10 * np.log10((np.sum(ref ** 2) + 1e-20) / (np.sum((ref - est) ** 2) + 1e-20)))
