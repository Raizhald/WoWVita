#!/usr/bin/env python3
"""w3nn.py - INT8 model container, quantisation and a bit-exact reference forward pass.

The C++ runtime (net/offline/offline_nn.cpp) and this module implement the same fixed-point
expressions, so a vector produced here is byte-for-byte reproducible on the console:

    hidden[h] = clamp( round_div_127( (sum_i x[i] * W1[h][i]) << shift1 ) + B1[h] ), 0, 127 )
    out[o]    = clamp( round_div_127( (sum_h hidden[h] * W2[o][h]) << shift2 ) + B2[o] ), -127, 127 )
    round_div_127(n) = (n + 63) // 127  for n >= 0,  -((-n + 63) // 127) otherwise

Weights are INT8 in [-127, 127]; per-layer shifts absorb the training weight scale so the
accumulators can stay INT32 without floating point. Biases are stored as INT32 in units of
127 * 127 * bias_real.

File layout ("W3NN"):
    u32 magic 'W3NN' (0x4E4E3357)
    u16 version (=1) | u16 inputs | u16 hidden | u16 outputs
    u8 shift1 | u8 shift2 | u16 reserved
    u32 checksum (FNV-1a of the payload)
    payload: W1 int8[hidden][inputs], B1 int32[hidden],
             W2 int8[outputs][hidden], B2 int32[outputs]
"""
import math
import struct

MAGIC = 0x4E4E3357
VERSION = 1
INPUTS = 24
HIDDEN = 16
OUTPUTS = 8
ACTIONS = ["attack", "ability", "defensive", "heal", "interrupt", "reposition", "retreat", "hold"]


def div127(n):
    """C-style symmetric division by 127 (truncation toward zero), not Python's floor."""
    return (n + 63) // 127 if n >= 0 else -((-n + 63) // 127)


def fnv(data):
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def layer_shift(weights):
    """Smallest k with 2**k >= max|w|, so int8 weights keep the full training range."""
    m = max((abs(w) for row in weights for w in row), default=0.0)
    if m <= 1.0:
        return 0
    return max(0, int(math.ceil(math.log2(m))))


def quantize(w1, b1, w2, b2):
    s1 = layer_shift(w1)
    s2 = layer_shift(w2)
    f1 = float(1 << s1)
    f2 = float(1 << s2)
    q1 = [[max(-127, min(127, int(round(w * 127.0 / f1)))) for w in row] for row in w1]
    q2 = [[max(-127, min(127, int(round(w * 127.0 / f2)))) for w in row] for row in w2]
    qb1 = [int(round(b * 127.0 * 127.0)) for b in b1]
    qb2 = [int(round(b * 127.0 * 127.0)) for b in b2]
    return {"w1": q1, "b1": qb1, "w2": q2, "b2": qb2, "s1": s1, "s2": s2}


def to_bytes(model):
    payload = bytearray()
    for row in model["w1"]:
        payload += struct.pack("<" + "b" * INPUTS, *row)
    payload += struct.pack("<" + "i" * HIDDEN, *model["b1"])
    for row in model["w2"]:
        payload += struct.pack("<" + "b" * HIDDEN, *row)
    payload += struct.pack("<" + "i" * OUTPUTS, *model["b2"])
    head = struct.pack("<IHHHHBBHI", MAGIC, VERSION, INPUTS, HIDDEN, OUTPUTS, model["s1"], model["s2"], 0, fnv(payload))
    return head + bytes(payload)


def from_bytes(blob):
    magic, version, nin, nhid, nout, s1, s2, _res, checksum = struct.unpack_from("<IHHHHBBHI", blob, 0)
    if magic != MAGIC or version != VERSION or nin != INPUTS or nhid != HIDDEN or nout != OUTPUTS:
        raise ValueError("not a supported W3NN model")
    payload = blob[20:]
    if fnv(payload) != checksum:
        raise ValueError("model checksum mismatch")
    at = 0
    w1 = []
    for _ in range(HIDDEN):
        w1.append(list(struct.unpack_from("<" + "b" * INPUTS, payload, at)))
        at += INPUTS
    b1 = list(struct.unpack_from("<" + "i" * HIDDEN, payload, at))
    at += 4 * HIDDEN
    w2 = []
    for _ in range(OUTPUTS):
        w2.append(list(struct.unpack_from("<" + "b" * HIDDEN, payload, at)))
        at += HIDDEN
    b2 = list(struct.unpack_from("<" + "i" * OUTPUTS, payload, at))
    return {"w1": w1, "b1": b1, "w2": w2, "b2": b2, "s1": s1, "s2": s2}


def scores(model, x):
    hidden = []
    for h in range(HIDDEN):
        acc = 0
        row = model["w1"][h]
        for i in range(INPUTS):
            acc += x[i] * row[i]
        acc = acc * (1 << model["s1"]) + model["b1"][h]
        v = div127(acc)
        hidden.append(0 if v < 0 else (127 if v > 127 else v))
    out = []
    for o in range(OUTPUTS):
        acc = 0
        row = model["w2"][o]
        for h in range(HIDDEN):
            acc += hidden[h] * row[h]
        acc = acc * (1 << model["s2"]) + model["b2"][o]
        v = div127(acc)
        out.append(-127 if v < -127 else (127 if v > 127 else v))
    return out


def infer(model, x):
    s = scores(model, x)
    return max(range(OUTPUTS), key=lambda i: s[i])


def golden_text(model, samples):
    lines = ["# W3NN golden vectors: 24 inputs | 8 scores | argmax"]
    for x in samples:
        s = scores(model, x)
        lines.append(",".join(map(str, x)) + "|" + ",".join(map(str, s)) + "|" + str(infer(model, x)))
    return "\n".join(lines) + "\n"


def verify_golden(model, text):
    bad = 0
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        a, b, c = line.split("|")
        x = [int(v) for v in a.split(",")]
        want = [int(v) for v in b.split(",")]
        if scores(model, x) != want or infer(model, x) != int(c):
            bad += 1
    return bad
