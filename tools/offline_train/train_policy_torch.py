#!/usr/bin/env python3
"""train_policy_torch.py - the same imitation pipeline with PyTorch (preferred when installed).

    python tools/offline_train/train_policy_torch.py \
        --data build/offline/teacher.csv --out build/offline/party.w3nn --epochs 60

The exported file is identical in format to the stdlib trainer's: INT8 weights, per-layer shifts,
checksum, plus `.golden` bit-exact vectors. Keeping training in PyTorch and inference in 512 MACs of
C++ is the point: training performance is never a 3DS metric.
"""
import argparse
import json
import sys

import w3nn
from train_policy import load_rows, normalize_hidden, normalize_output


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--val", type=float, default=0.15)
    args = ap.parse_args()

    try:
        import torch
        import torch.nn as nn
    except ImportError:
        print("PyTorch is not installed; use train_policy.py (stdlib) for the same output format")
        return 2

    rows = load_rows(args.data)
    if len(rows) < 50:
        print(f"not enough samples ({len(rows)}); run w3doffline --gen-data first")
        return 1
    torch.manual_seed(args.seed)
    torch.set_num_threads(1)
    cut = int(len(rows) * (1.0 - args.val))
    train_rows, val_rows = rows[:cut], rows[cut:]

    x = torch.tensor([[v / 127.0 for v in r[0]] for r in train_rows], dtype=torch.float32)
    y = torch.tensor([r[1] for r in train_rows], dtype=torch.long)
    xv = torch.tensor([[v / 127.0 for v in r[0]] for r in val_rows], dtype=torch.float32)
    yv = torch.tensor([r[1] for r in val_rows], dtype=torch.long)

    model = nn.Sequential(nn.Linear(24, 16), nn.ReLU(), nn.Linear(16, 8))
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    lossf = nn.CrossEntropyLoss()
    n = len(train_rows)
    for ep in range(args.epochs):
        model.train()
        perm = torch.randperm(n)
        total = 0.0
        for i in range(0, n, args.batch):
            idx = perm[i:i + args.batch]
            opt.zero_grad()
            out = model(x[idx])
            loss = lossf(out, y[idx])
            loss.backward()
            opt.step()
            total += float(loss) * len(idx)
        if ep % 10 == 0 or ep == args.epochs - 1:
            model.eval()
            with torch.no_grad():
                acc = float((model(xv).argmax(1) == yv).float().mean())
            print(f"  epoch {ep + 1:3d}: loss {total / n:.4f} val acc {acc * 100:.1f}%")

    # Export the trained weights through the same quantiser as the stdlib trainer.
    w1 = model[0].weight.detach().tolist()
    b1 = model[0].bias.detach().tolist()
    w2 = model[2].weight.detach().tolist()
    b2 = model[2].bias.detach().tolist()
    net = {"w1": w1, "b1": b1, "w2": w2, "b2": b2}
    # Keep the fixed-point range valid: scale hidden units and logits exactly like the stdlib path.
    net = normalize_hidden(net, train_rows)
    net = normalize_output(net, train_rows)
    q = w3nn.quantize(net["w1"], net["b1"], net["w2"], net["b2"])
    blob = w3nn.to_bytes(q)
    open(args.out, "wb").write(blob)
    back = w3nn.from_bytes(blob)
    good = sum(1 for r in val_rows if w3nn.infer(back, r[0]) == r[1])
    acc = good / max(1, len(val_rows))
    samples = [list(r[0]) for r in val_rows[:24]] + [[0] * 24, [127] * 24]
    open(args.out + ".golden", "w", encoding="utf-8").write(w3nn.golden_text(back, samples))
    json.dump({"samples": len(rows), "int8_accuracy": acc, "shifts": [q["s1"], q["s2"]],
               "actions": w3nn.ACTIONS, "seed": args.seed, "epochs": args.epochs, "lr": args.lr},
              open(args.out + ".report", "w"), indent=2)
    print(f"int8 validation accuracy: {acc * 100:.1f}%  -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
