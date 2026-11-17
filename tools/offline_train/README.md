# PartyBot neural policy — training pipeline

The runtime controller on the console is the deterministic Utility AI in
`net/offline/offline_bots.cpp`. This folder trains the optional tiny MLP that can replace its
*tactical category choice*, while the legal-action mask and the emergency rules stay deterministic.

## 1. Record the teacher

```bash
build/offline/w3doffline --gen-data teacher.csv 60 build/offline/offline_sample.w3do
```

`--gen-data` runs the real `World` and Party Director (never the model), randomises the encounter
(including a fifth of episodes starting wounded), and records one CSV row per subsampled bot
decision:

```
f0..f23, action, legal_mask, reward, role
```

The reward is the episode outcome (+1 party survived, −1 player died, 0 otherwise) written into every
row of that episode.

## 2. Train and export

```bash
python tools/offline_train/train_policy.py \
  --data build/offline/teacher.csv --out build/offline/party.w3nn --epochs 40
```

or, with PyTorch installed:

```bash
python tools/offline_train/train_policy_torch.py \
  --data build/offline/teacher.csv --out build/offline/party.w3nn --epochs 60
```

Both produce the same `W3NN` file, the bit-exact `.golden` vectors and a `.report` JSON. The stdlib
trainer exists because the development PC used for this milestone has no PyTorch; it implements a
24→16→8 MLP with momentum SGD, then quantises with the helper both paths share (`w3nn.py`).

## 3. Verify

```bash
python - <<'PY'
import sys; sys.path.insert(0, "tools/offline_train")
import w3nn
m = w3nn.from_bytes(open("build/offline/party.w3nn", "rb").read())
print("mismatches", w3nn.verify_golden(m, open("build/offline/party.w3nn.golden").read()))
PY
```

`net/tests/run_offline.sh` does this automatically and then runs the C++ runtime against the same
golden vectors.

Copy `party.w3nn` to `sdmc:/3ds/w3dproto/offline/party.w3nn` to enable it on the console. If the file
is missing or invalid, the deterministic AI is used; a bad model can never stop the game.

A trained sample policy (the one produced by the command above on the sample dataset, 628 bytes) is
checked in as `tools/offline_train/sample/party.w3nn`, together with its `.golden` vectors and
`.report`. It imitates the deterministic teacher at 99.2 % on the held-out split.

## Design notes

* Inputs are the documented 24-feature contract, INT8 in `[0,127]` (fraction of 1).
* Weights are INT8 with a per-layer power-of-two shift; biases are INT32 in units of 127²·b.
* The legal-action mask is applied after inference. Emergency heals, taunts, retreats and invalid
  targets are decided before the model runs.
* Training never sees a framebuffer; it sees the same tactical features the runtime has.
* The benchmark (`w3doffline --bench`) compares deterministic and MLP controllers on held-out
  encounters. On the sample dataset both complete 20/20; no behavioural advantage was demonstrated,
  so the deterministic controller remains the default.
