#!/usr/bin/env python3
"""train_policy.py - imitation learning for the tiny PartyBot policy (stdlib only).

Teacher: the deterministic Utility AI in net/offline/offline_bots.cpp, recorded by
`w3doffline --gen-data`. This script trains Model A (24 -> 16 -> 8) with mini-batch SGD,
quantises it to INT8 with per-layer shifts (w3nn.py) and writes:

    <out>.w3nn          the runtime model (versioned, checksummed)
    <out>.w3nn.golden   bit-exact vectors for the C++ test
    <out>.w3nn.report   accuracy, quantisation loss, confusion matrix, seeds

The same exporter is used by train_policy_torch.py when PyTorch is available.

    python tools/offline_train/train_policy.py --data teacher.csv --out party.w3nn
"""
import argparse
import json
import math
import random
import sys

import w3nn

EXAMPLE_ACTIONS = w3nn.ACTIONS


def load_rows(path):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            if len(parts) != 28:
                continue
            feat = [max(0, min(127, int(v))) for v in parts[:24]]
            action = int(parts[24])
            mask = int(parts[25])
            reward = int(parts[26])
            role = int(parts[27])
            if action < 0 or action >= 8:
                continue
            rows.append((feat, action, mask, reward, role))
    return rows


def init_net(rng, scale=0.35):
    w1 = [[rng.uniform(-scale, scale) for _ in range(w3nn.INPUTS)] for _ in range(w3nn.HIDDEN)]
    b1 = [0.0] * w3nn.HIDDEN
    w2 = [[rng.uniform(-scale, scale) for _ in range(w3nn.HIDDEN)] for _ in range(w3nn.OUTPUTS)]
    b2 = [0.0] * w3nn.OUTPUTS
    return {"w1": w1, "b1": b1, "w2": w2, "b2": b2}


def forward(net, x):
    """x: 24 floats in 0..1. Returns (hidden, logits)."""
    hidden = []
    for h in range(w3nn.HIDDEN):
        acc = net["b1"][h]
        row = net["w1"][h]
        for i in range(w3nn.INPUTS):
            acc += x[i] * row[i]
        hidden.append(acc if acc > 0.0 else 0.0)
    logits = []
    for o in range(w3nn.OUTPUTS):
        acc = net["b2"][o]
        row = net["w2"][o]
        for h in range(w3nn.HIDDEN):
            acc += hidden[h] * row[h]
        logits.append(acc)
    return hidden, logits


def softmax(logits):
    m = max(logits)
    e = [math.exp(v - m) for v in logits]
    s = sum(e)
    return [v / s for v in e]


def accuracy(net, rows):
    good = 0
    for feat, action, *_ in rows:
        _, logits = forward(net, [v / 127.0 for v in feat])
        p = softmax(logits)
        if max(range(8), key=lambda i: p[i]) == action:
            good += 1
    return good / max(1, len(rows))


def train(net, rows, epochs, lr, rng, verbose=True):
    momentum = {k: ([[0.0] * len(row) for row in v] if isinstance(v[0], list) else [0.0] * len(v))
                for k, v in net.items()}
    batch = 32
    order = list(range(len(rows)))
    for ep in range(epochs):
        rng.shuffle(order)
        total_loss = 0.0
        for start in range(0, len(order), batch):
            idx = order[start:start + batch]
            gw = {k: ([[0.0] * len(row) for row in v] if isinstance(v[0], list) else [0.0] * len(v))
                  for k, v in net.items()}
            for j in idx:
                feat, action = rows[j][0], rows[j][1]
                x = [v / 127.0 for v in feat]
                hidden, logits = forward(net, x)
                p = softmax(logits)
                total_loss -= math.log(max(1e-9, p[action]))
                d = p[:]
                d[action] -= 1.0
                for o in range(w3nn.OUTPUTS):
                    gw["b2"][o] += d[o]
                    for h in range(w3nn.HIDDEN):
                        gw["w2"][o][h] += d[o] * hidden[h]
                dh = [0.0] * w3nn.HIDDEN
                for h in range(w3nn.HIDDEN):
                    s = 0.0
                    for o in range(w3nn.OUTPUTS):
                        s += d[o] * net["w2"][o][h]
                    dh[h] = s if hidden[h] > 0.0 else 0.0
                for h in range(w3nn.HIDDEN):
                    gw["b1"][h] += dh[h]
                    for i in range(w3nn.INPUTS):
                        gw["w1"][h][i] += dh[h] * x[i]
            scale = 1.0 / max(1, len(idx))
            for k in net:
                gk, mk, vk = gw[k], momentum[k], net[k]
                if isinstance(vk[0], list):
                    for a in range(len(vk)):
                        for b in range(len(vk[a])):
                            g = gk[a][b] * scale
                            mk[a][b] = 0.9 * mk[a][b] - lr * g
                            vk[a][b] += mk[a][b]
                else:
                    for a in range(len(vk)):
                        g = gk[a] * scale
                        mk[a] = 0.9 * mk[a] - lr * g
                        vk[a] += mk[a]
        if verbose and (ep % 5 == 0 or ep == epochs - 1):
            print(f"  epoch {ep + 1:3d}: loss {total_loss / max(1, len(rows)):.4f} "
                  f"train acc {accuracy(net, rows) * 100:.1f}%")
    return net


def normalize_hidden(net, rows):
    """Scale every hidden unit so its pre-activation stays inside the INT8 Q0.7 range.

    The output layer is scaled by the inverse, so the float network's logits do not change; the
    quantised runtime then sees hidden values that are never clipped by the 127 bound.
    """
    peaks = [0.0] * w3nn.HIDDEN
    for feat, *_ in rows:
        x = [v / 127.0 for v in feat]
        for h in range(w3nn.HIDDEN):
            acc = net["b1"][h]
            row = net["w1"][h]
            for i in range(w3nn.INPUTS):
                acc += x[i] * row[i]
            if acc > peaks[h]:
                peaks[h] = acc
    for h in range(w3nn.HIDDEN):
        k = max(1.0, peaks[h] * 1.02)
        if k <= 1.0000001:
            continue
        net["b1"][h] /= k
        for i in range(w3nn.INPUTS):
            net["w1"][h][i] /= k
        for o in range(w3nn.OUTPUTS):
            net["w2"][o][h] *= k
    return net


def normalize_output(net, rows):
    """Scale the logits into [-1, 1] so the INT8 output scores never saturate.

    A uniform positive scale does not change the argmax, and the runtime only needs the order.
    """
    peak = 0.0
    for feat, *_ in rows:
        x = [v / 127.0 for v in feat]
        _, logits = forward(net, x)
        for v in logits:
            peak = max(peak, abs(v))
    k = max(1.0, peak * 1.02)
    for o in range(w3nn.OUTPUTS):
        net["b2"][o] /= k
        for h in range(w3nn.HIDDEN):
            net["w2"][o][h] /= k
    return net


def confusion(net, rows):
    m = [[0] * 8 for _ in range(8)]
    for feat, action, *_ in rows:
        _, logits = forward(net, [v / 127.0 for v in feat])
        p = softmax(logits)
        m[action][max(range(8), key=lambda i: p[i])] += 1
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--lr", type=float, default=0.12)
    ap.add_argument("--hidden", type=int, default=16)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--val", type=float, default=0.15)
    args = ap.parse_args()
    if args.hidden != w3nn.HIDDEN:
        print(f"note: this runtime is fixed at {w3nn.HIDDEN} hidden units; ignoring --hidden {args.hidden}")

    rows = load_rows(args.data)
    if len(rows) < 50:
        print(f"not enough samples ({len(rows)}); run w3doffline --gen-data first")
        return 1
    rng = random.Random(args.seed)
    rng.shuffle(rows)
    cut = int(len(rows) * (1.0 - args.val))
    train_rows, val_rows = rows[:cut], rows[cut:]
    print(f"samples: {len(rows)} (train {len(train_rows)}, val {len(val_rows)})")
    counts = [0] * 8
    for r in train_rows:
        counts[r[1]] += 1
    print("teacher actions: " + ", ".join(f"{EXAMPLE_ACTIONS[i]}={counts[i]}" for i in range(8)))

    net = init_net(random.Random(args.seed + 1))
    train(net, train_rows, args.epochs, args.lr, rng)
    net = normalize_hidden(net, train_rows)
    net = normalize_output(net, train_rows)
    float_val = accuracy(net, val_rows)
    print(f"float validation accuracy: {float_val * 100:.1f}%")

    model = w3nn.quantize(net["w1"], net["b1"], net["w2"], net["b2"])
    blob = w3nn.to_bytes(model)
    with open(args.out, "wb") as f:
        f.write(blob)
    # Round-trip through the runtime format: the quantised weights must reproduce the same argmax.
    back = w3nn.from_bytes(blob)
    q_good = 0
    for feat, action, *_ in val_rows:
        if w3nn.infer(back, feat) == action:
            q_good += 1
    quant_val = q_good / max(1, len(val_rows))
    print(f"int8 validation accuracy: {quant_val * 100:.1f}%  (shifts {model['s1']}/{model['s2']})")

    samples = [list(r[0]) for r in val_rows[:24]]
    # a few synthetic corners too: everything zero, everything max, role/class extremes
    samples.append([0] * 24)
    samples.append([127] * 24)
    samples.append([127 if i % 2 else 0 for i in range(24)])
    samples.append([0] * 20 + [127, 127, 127, 0])
    with open(args.out + ".golden", "w", encoding="utf-8") as f:
        f.write(w3nn.golden_text(back, samples))
    bad = w3nn.verify_golden(back, open(args.out + ".golden", encoding="utf-8").read())
    print(f"golden vectors: {len(samples)}, mismatches {bad}")

    cm = confusion(net, val_rows)
    report = {
        "samples": len(rows), "train": len(train_rows), "validation": len(val_rows),
        "float_accuracy": float_val, "int8_accuracy": quant_val,
        "shifts": [model["s1"], model["s2"]], "seed": args.seed, "epochs": args.epochs,
        "lr": args.lr, "actions": EXAMPLE_ACTIONS,
        "teacher_action_counts": counts, "confusion_teacher_rows": cm,
        "note": "The legal-action mask and emergency rules always override the model at runtime.",
    }
    with open(args.out + ".report", "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"wrote {args.out} ({len(blob)} bytes), golden vectors and report")
    return 0


if __name__ == "__main__":
    sys.exit(main())
