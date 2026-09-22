#!/usr/bin/env python3
"""Compare the samples of two WAV files.

Usage:
    python3 tools/compare_wav.py a.wav b.wav [--tolerance 0] [--hop 512]
                                             [--max-regions 20] [--merge-gap 0]

Reports whether the two files are sample-identical. If they are not, it lists
where they differ as contiguous regions, in samples, seconds and PitchNet
analysis frames (hop 512 at 44.1 kHz by default), so a region can be matched
against the commit=[start,end) ranges in PitchNet's debug log.

Supports PCM 8/16/24/32-bit, IEEE float 32/64-bit and WAVE_FORMAT_EXTENSIBLE.
Uses only the standard library; numpy is used automatically if installed.

Exit code: 0 = identical (or within --tolerance), 1 = different, 2 = error.
"""

import argparse
import struct
import sys

try:
    import numpy as np
except ImportError:  # pragma: no cover - optional speed-up
    np = None

WAVE_FORMAT_PCM = 0x0001
WAVE_FORMAT_IEEE_FLOAT = 0x0003
WAVE_FORMAT_EXTENSIBLE = 0xFFFE
ANALYSIS_RATE = 44100  # PitchNet's analysis frame grid is HOP_SIZE at 44.1 kHz


class WavInfo:
    def __init__(self, path, fmt, channels, rate, bits, data):
        self.path = path
        self.format = fmt
        self.channels = channels
        self.rate = rate
        self.bits = bits
        self.data = data
        self.bytes_per_sample = bits // 8
        self.frames = len(data) // (self.bytes_per_sample * channels)

    def describe(self):
        kind = "float" if self.format == WAVE_FORMAT_IEEE_FLOAT else "PCM"
        return (f"{self.rate} Hz, {self.channels} ch, {self.bits}-bit {kind}, "
                f"{self.frames} frames ({self.frames / self.rate:.3f} s)")


def read_wav(path):
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < 12 or raw[0:4] not in (b"RIFF", b"RF64") or raw[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")

    fmt = channels = rate = bits = None
    data = None
    pos = 12
    while pos + 8 <= len(raw):
        chunk_id = raw[pos:pos + 4]
        size = struct.unpack_from("<I", raw, pos + 4)[0]
        body = pos + 8
        if chunk_id == b"fmt ":
            fmt, channels, rate = struct.unpack_from("<HHI", raw, body)
            bits = struct.unpack_from("<H", raw, body + 14)[0]
            if fmt == WAVE_FORMAT_EXTENSIBLE and size >= 40:
                # First two bytes of the SubFormat GUID hold the real format.
                fmt = struct.unpack_from("<H", raw, body + 24)[0]
        elif chunk_id == b"data":
            end = len(raw) if size == 0xFFFFFFFF else min(len(raw), body + size)
            data = raw[body:end]
        pos = body + size + (size & 1)  # chunks are word-aligned

    if fmt is None or data is None:
        raise ValueError(f"{path}: missing fmt or data chunk")
    if fmt not in (WAVE_FORMAT_PCM, WAVE_FORMAT_IEEE_FLOAT):
        raise ValueError(f"{path}: unsupported format tag 0x{fmt:04X}")
    if fmt == WAVE_FORMAT_PCM and bits not in (8, 16, 24, 32):
        raise ValueError(f"{path}: unsupported PCM bit depth {bits}")
    if fmt == WAVE_FORMAT_IEEE_FLOAT and bits not in (32, 64):
        raise ValueError(f"{path}: unsupported float bit depth {bits}")
    return WavInfo(path, fmt, channels, rate, bits, data)


def decode(info):
    """Return interleaved samples as floats in [-1, 1] (list or numpy array)."""
    data, bits = info.data, info.bits
    usable = len(data) - len(data) % info.bytes_per_sample
    data = data[:usable]

    if info.format == WAVE_FORMAT_IEEE_FLOAT:
        code = "f" if bits == 32 else "d"
        if np is not None:
            return np.frombuffer(data, dtype="<" + code).astype(np.float64)
        return list(struct.unpack("<%d%s" % (usable // (bits // 8), code), data))

    if bits == 8:  # unsigned
        if np is not None:
            return (np.frombuffer(data, dtype=np.uint8).astype(np.float64) - 128) / 128.0
        return [(b - 128) / 128.0 for b in data]
    if bits == 16:
        if np is not None:
            return np.frombuffer(data, dtype="<i2").astype(np.float64) / 32768.0
        return [v / 32768.0 for v in struct.unpack("<%dh" % (usable // 2), data)]
    if bits == 32:
        if np is not None:
            return np.frombuffer(data, dtype="<i4").astype(np.float64) / 2147483648.0
        return [v / 2147483648.0 for v in struct.unpack("<%di" % (usable // 4), data)]

    # 24-bit little-endian signed
    if np is not None:
        b = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 0x800000, v - 0x1000000, v)
        return v.astype(np.float64) / 8388608.0
    out = []
    for i in range(0, usable, 3):
        v = data[i] | (data[i + 1] << 8) | (data[i + 2] << 16)
        if v >= 0x800000:
            v -= 0x1000000
        out.append(v / 8388608.0)
    return out


def differing_frames(a, b, channels, frames, tolerance):
    """Per-frame flag (any channel differs by more than tolerance), plus stats."""
    count = frames * channels
    if np is not None:
        diff = np.abs(a[:count] - b[:count])
        over = diff > tolerance
        per_frame = over.reshape(frames, channels).any(axis=1)
        max_diff = float(diff.max()) if count else 0.0
        max_at = int(diff.argmax()) if count else 0
        return per_frame, int(over.sum()), max_diff, max_at

    per_frame = [False] * frames
    n_over = 0
    max_diff = 0.0
    max_at = 0
    for i in range(count):
        d = abs(a[i] - b[i])
        if d > max_diff:
            max_diff, max_at = d, i
        if d > tolerance:
            n_over += 1
            per_frame[i // channels] = True
    return per_frame, n_over, max_diff, max_at


def regions_from_flags(flags, merge_gap):
    regions = []
    start = None
    last = None
    for i, flag in enumerate(flags):
        if not flag:
            continue
        if start is None:
            start = last = i
        elif i - last - 1 <= merge_gap:
            last = i
        else:
            regions.append((start, last + 1))
            start = last = i
    if start is not None:
        regions.append((start, last + 1))
    return regions


def db(value):
    if value <= 0:
        return "-inf dBFS"
    import math
    return f"{20 * math.log10(value):.1f} dBFS"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--tolerance", type=float, default=0.0,
                        help="max absolute difference (full scale = 1.0) "
                             "still counted as equal; default 0 = exact")
    parser.add_argument("--hop", type=int, default=512,
                        help="PitchNet hop size for frame numbers (default 512)")
    parser.add_argument("--max-regions", type=int, default=20,
                        help="how many differing regions to list (default 20)")
    parser.add_argument("--merge-gap", type=int, default=0,
                        help="merge differing regions separated by at most this "
                             "many equal samples (default 0)")
    args = parser.parse_args()

    try:
        a = read_wav(args.a)
        b = read_wav(args.b)
    except (OSError, ValueError, struct.error) as err:
        print(f"error: {err}", file=sys.stderr)
        return 2

    print(f"A: {a.path}\n   {a.describe()}")
    print(f"B: {b.path}\n   {b.describe()}")

    if a.channels != b.channels:
        print("DIFFERENT: channel counts differ, samples not compared")
        return 1
    if a.rate != b.rate:
        print("note: sample rates differ; comparing sample by sample anyway")

    same_format = (a.format, a.bits) == (b.format, b.bits)
    if same_format and a.data == b.data and args.tolerance == 0.0:
        print("IDENTICAL: every sample matches exactly")
        return 0

    frames = min(a.frames, b.frames)
    sa, sb = decode(a), decode(b)
    flags, n_over, max_diff, max_at = differing_frames(
        sa, sb, a.channels, frames, args.tolerance)

    length_note = ""
    if a.frames != b.frames:
        length_note = (f"lengths differ by {abs(a.frames - b.frames)} frames; "
                       f"compared the first {frames}")
        print(f"note: {length_note}")

    if n_over == 0:
        if a.frames != b.frames:
            print("DIFFERENT: overlapping samples match, but the lengths differ")
            return 1
        if args.tolerance > 0.0:
            print(f"EQUAL WITHIN TOLERANCE {args.tolerance} "
                  f"(max difference {max_diff:.3g}, {db(max_diff)})")
        else:
            print("IDENTICAL: every sample matches exactly "
                  "(different encodings, same values)")
        return 0

    rate = a.rate
    samples_per_frame = args.hop * rate / ANALYSIS_RATE
    max_frame, max_ch = divmod(max_at, a.channels)
    print(f"DIFFERENT: {n_over} of {frames * a.channels} samples differ "
          f"(tolerance {args.tolerance})")
    print(f"max difference {max_diff:.6g} ({db(max_diff)}) at sample {max_frame} "
          f"({max_frame / rate:.4f} s), channel {max_ch}")

    regions = regions_from_flags(flags, args.merge_gap)
    print(f"{len(regions)} differing region(s):")
    print("   samples [start,end)        seconds [start,end)      "
          "PitchNet frames [start,end)")
    for start, end in regions[:args.max_regions]:
        f0 = int(start // samples_per_frame)
        f1 = int(-(-end // samples_per_frame))  # ceil
        print(f"   [{start},{end})".ljust(30)
              + f"[{start / rate:.4f},{end / rate:.4f})".ljust(25)
              + f"[{f0},{f1})  ({end - start} samples)")
    if len(regions) > args.max_regions:
        print(f"   ... {len(regions) - args.max_regions} more "
              f"(raise --max-regions to list them)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
