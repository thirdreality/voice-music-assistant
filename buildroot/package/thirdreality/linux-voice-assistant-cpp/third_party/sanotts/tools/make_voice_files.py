#!/usr/bin/env python3
"""Convert an upstream sanoTTS piperlite voice into LVA's voice files.

Input:  <src>/front_f32.bin, <src>/dec_f32.bin (upstream web/voices/<voice>)
Output: <dst>/front_f32.bin  duration + acoustic students, upstream bytes
                             (int8 or fp16 here would shift durations)
        <dst>/dec_q8.bin     decoder, conv weights per-output-channel int8

dec_q8.bin layout (little-endian, every section 16-byte aligned), parsed by
snt_piperlite_init_q8():
  u32 magic 'LQ8D' (0x4438514C), u32 version 1,
  u32 meta_bytes, u32 n_tensors
  meta.bin (upstream bytes, unchanged; offsets count fp32 floats)
  n_tensors x { u32 kind, u32 data_off, u32 scale_off, u32 n_oc }
    kind 0: fp32, data_off -> floats
    kind 1: int8, data_off -> int8 in the fp32 element order,
            scale_off -> n_oc fp32 scales (value = q * scale[oc])
  payload
"""

import argparse
import pathlib
import struct

import numpy as np

UP_K = (16, 16, 8)
MAGIC = 0x4438514C
MIN_QUANT_FLOATS = 1024  # small tensors (bias, post conv) stay fp32


def align16(n):
    return (n + 15) & ~15


def split(blob, count_at, header):
    n = struct.unpack_from("<i", blob, count_at)[0]
    meta = header + 8 * n
    slots = [struct.unpack_from("<ii", blob, header + 8 * i) for i in range(n)]
    return blob[:meta], slots, np.frombuffer(blob[meta:], "<f4")


def conv_shapes(meta):
    """slot -> (shape, oc_axis) for the decoder conv weights."""
    in_ch, c0, c1, c2, c3 = struct.unpack_from("<5i", meta, 8)
    shapes = {0: ((c0, in_ch, 7), 0)}
    ins, outs, base = (c0, c1, c2), (c1, c2, c3), 2
    for s in range(3):
        shapes[base] = ((ins[s], outs[s], UP_K[s]), 1)  # ConvTranspose1d
        for br, k in enumerate((3, 5, 7)):
            shapes[base + 2 + 4 * br] = ((outs[s], outs[s], k), 0)
            shapes[base + 4 + 4 * br] = ((outs[s], outs[s], k), 0)
        base += 14
    return shapes


def write_front(src, dst):
    blob = src.read_bytes()
    split(blob, 68, 72)  # format check
    dst.write_bytes(blob)


def write_dec(src, dst):
    meta, slots, w = split(src.read_bytes(), 44, 48)
    shapes = conv_shapes(meta)
    table, payload = [], bytearray()
    base = align16(16 + len(meta)) + align16(16 * len(slots))
    for i, (off, size) in enumerate(slots):
        t = w[off:off + size]
        if i in shapes and size >= MIN_QUANT_FLOATS:
            shape, ax = shapes[i]
            t = t.reshape(shape)
            red = tuple(a for a in range(3) if a != ax)
            sc = (np.abs(t).max(axis=red) / 127.0).astype("<f4")
            sc[sc == 0] = 1.0
            bshape = [1, 1, 1]
            bshape[ax] = -1
            q = np.clip(np.round(t / sc.reshape(bshape)), -127, 127)
            data_off = base + len(payload)
            payload += q.astype("<i1").tobytes()
            payload += b"\0" * (align16(len(payload)) - len(payload))
            scale_off = base + len(payload)
            payload += sc.tobytes()
            table.append((1, data_off, scale_off, sc.size))
        else:
            data_off = base + len(payload)
            payload += t.astype("<f4").tobytes()
            table.append((0, data_off, 0, 0))
        payload += b"\0" * (align16(len(payload)) - len(payload))
    out = bytearray(struct.pack("<4I", MAGIC, 1, len(meta), len(slots)))
    out += meta
    out += b"\0" * (align16(len(out)) - len(out))
    for rec in table:
        out += struct.pack("<4I", *rec)
    out += b"\0" * (align16(len(out)) - len(out))
    assert len(out) == base
    out += payload
    dst.write_bytes(bytes(out))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", type=pathlib.Path)
    ap.add_argument("dst", type=pathlib.Path)
    a = ap.parse_args()
    a.dst.mkdir(parents=True, exist_ok=True)
    write_front(a.src / "front_f32.bin", a.dst / "front_f32.bin")
    write_dec(a.src / "dec_f32.bin", a.dst / "dec_q8.bin")
    for f in sorted(a.dst.iterdir()):
        print(f"{f}: {f.stat().st_size} bytes")


if __name__ == "__main__":
    main()
