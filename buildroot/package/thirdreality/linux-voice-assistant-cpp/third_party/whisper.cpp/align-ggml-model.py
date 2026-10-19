#!/usr/bin/env python3
"""Rewrite a whisper.cpp ggml model so every tensor's data is 32-byte aligned.

Needed by whisper_init_from_file_mmap_no_state() (LVA patch), which points
weight tensors straight into a read-only mmap of the file. Alignment is
achieved by NUL-padding tensor names; the patched loader strips the padding.
The rewritten file still loads with the regular (copying) loader.

Usage: align-ggml-model.py <in.bin> <out.bin>
"""
import struct
import sys

ALIGN = 32
# ggml type -> (block size, bytes per block); covers the types whisper uses.
TYPE_SIZE = {0: (1, 4), 1: (1, 2), 2: (32, 18), 3: (32, 20), 6: (32, 22),
             7: (32, 24), 8: (32, 34)}


def main(src, dst):
    d = open(src, "rb").read()
    out = bytearray()
    off = 0

    def take(n):
        nonlocal off
        chunk = d[off:off + n]
        off += n
        return chunk

    out += take(4 + 11 * 4)                          # magic + hparams
    n_mel, n_fft = struct.unpack_from("<ii", d, off)
    out += take(8 + 4 * n_mel * n_fft)               # mel filters
    (n_vocab,) = struct.unpack_from("<i", d, off)
    out += take(4)
    for _ in range(n_vocab):
        (ln,) = struct.unpack_from("<i", d, off)
        out += take(4 + ln)

    n_tensors = 0
    while off < len(d):
        n_dims, name_len, ttype = struct.unpack_from("<iii", d, off)
        off += 12
        ne = struct.unpack_from("<%di" % n_dims, d, off)
        off += 4 * n_dims
        name = take(name_len)
        blk, bpb = TYPE_SIZE[ttype]
        n = 1
        for e in ne:
            n *= e
        data = take(n // blk * bpb)

        header = 12 + 4 * n_dims
        pad = (-(len(out) + header + name_len)) % ALIGN
        name = name + b"\0" * pad
        out += struct.pack("<iii", n_dims, len(name), ttype)
        out += struct.pack("<%di" % n_dims, *ne)
        out += name
        assert len(out) % ALIGN == 0
        out += data
        n_tensors += 1

    open(dst, "wb").write(out)
    print(f"{n_tensors} tensors aligned to {ALIGN} bytes: {dst} ({len(out)} bytes)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
