#!/usr/bin/env python3
"""WoWVita: a whole-continent offline world pack (.w3do) from a full CMaNGOS / vMaNGOS world database dump.

The upstream exporter (export_offline_data.py) reads small INSERT files with column lists. A full dump
(mysqldump: CREATE TABLE + one extended INSERT per line, no column list, often .sql.gz) is read here instead,
with the CMaNGOS classic-db and vMaNGOS column names. What it adds over the sample:

* every visible creature spawn of the map (no tile clipping needed: the client's spawn grid keeps only the
  creatures around the player in memory),
* creature stats, XP by level, loot with reference tables and money,
* hostility from the converter's factions.txt (FactionTemplate.dbc): red creatures attack, yellow ones only
  fight back (HOST pairs with bit 31 set, see net/offline/offline_core.hpp).

Spells, companions and the start point stay those of the sample (the offline core implements a documented
spell subset only).

    python3 export_world_sql.py --sql ClassicDB_1_12_1.sql.gz --factions factions.txt --map 0 --out offline.w3do
"""
import argparse
import gzip
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export_offline_data as _eod  # noqa: E402
from export_offline_data import (Bin, build_sample, read_pack, split_sql_values,  # noqa: E402
                                 validate_sections, write_pack, write_report)

_eod.TAGS = tuple(_eod.TAGS) + ("VEND", "QST2", "QLOT", "GOSS", "GOBJ", "GOSP", "GLOT", "GRAV", "TAVN", "ITX1", "EXPL", "QTRG", "MOVE", "CRAI", "AITX", "SPFX", "POOL", "LINK", "OUTF")   # WoWVita's vendor section (older readers skip unknown tags)

WANTED = {"creature_template", "creature", "creature_loot_template", "reference_loot_template",
          "item_template", "creature_model_info", "creature_spawn_entry", "npc_vendor", "npc_vendor_template",
          "quest_template", "creature_questrelation", "creature_involvedrelation", "gossip_menu", "npc_text",
          "gameobject", "gameobject_template", "gameobject_loot_template", "gameobject_questrelation",
          "gameobject_involvedrelation", "gameobject_spawn_entry", "world_safe_locs", "game_graveyard_zone", "areatrigger_tavern",
          "spell_template", "exploration_basexp", "areatrigger_involvedrelation",
          "creature_movement", "creature_movement_template", "creature_ai_scripts", "broadcast_text", "creature_ai_texts",
          "spawn_group", "spawn_group_spawn", "spawn_group_entry", "pool_creature", "pool_creature_template", "pool_template",
          "creature_linking", "creature_linking_template"}

# game object types the offline world shows: 1 button, 2 quest giver, 3 chest (and herbs / ore), 5 generic,
# 8 spell focus (campfires, anvils), 9 text, 10 goober (quest objects), 19 mailbox. Doors, traps, transports stay out.
GO_TYPES = {1, 2, 3, 5, 8, 9, 10, 19}

CREATE_RE = re.compile(r"^CREATE TABLE `(\w+)`")
COL_RE = re.compile(r"^\s+`(\w+)`")
INSERT_RE = re.compile(r"^INSERT INTO `(\w+)`\s*(\(([^)]*)\))?\s*VALUES\s*", re.I)


_ESC = {"n": "\n", "r": "", "t": "\t", "0": "", "Z": "", "\\": "\\", "'": "'", '"': '"'}


def sql_rows(text):
    """VALUES (...),(...) -> lists of values (str for quoted, int/float/None otherwise). Commas and parentheses
    inside strings and MySQL backslash escapes are handled (the upstream splitter cuts strings at commas)."""
    rows, i, n = [], 0, len(text)
    while i < n:
        if text[i] != "(":
            i += 1
            continue
        i += 1
        row, tok, quoted = [], [], False
        while i < n:
            ch = text[i]
            if ch == "'":
                quoted = True
                i += 1
                buf = []
                while i < n:
                    c = text[i]
                    if c == "\\" and i + 1 < n:
                        buf.append(_ESC.get(text[i + 1], text[i + 1]))
                        i += 2
                        continue
                    if c == "'":
                        if i + 1 < n and text[i + 1] == "'":
                            buf.append("'")
                            i += 2
                            continue
                        i += 1
                        break
                    buf.append(c)
                    i += 1
                tok = buf
                continue
            if ch == "," or ch == ")":
                v = "".join(tok)
                if quoted:
                    row.append(v)
                else:
                    v = v.strip()
                    if v.upper() == "NULL" or v == "":
                        row.append(None)
                    else:
                        try:
                            row.append(int(v))
                        except ValueError:
                            try:
                                row.append(float(v))
                            except ValueError:
                                row.append(v)
                tok, quoted = [], False
                i += 1
                if ch == ")":
                    break
                continue
            tok.append(ch)
            i += 1
        rows.append(row)
    return rows


def open_text(path):
    if path.endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8", errors="replace")
    return open(path, "r", encoding="utf-8", errors="replace")


def load_dump(paths):
    """{table: [row dict]} for the WANTED tables of one or more dumps (column names from CREATE TABLE)."""
    cols, tables = {}, {}
    for path in paths:
        with open_text(path) as f:
            cur = None
            for line in f:
                m = CREATE_RE.match(line)
                if m:
                    cur = m.group(1).lower()
                    cols[cur] = []
                    continue
                if cur is not None:
                    c = COL_RE.match(line)
                    if c:
                        cols[cur].append(c.group(1))
                        continue
                    if line.startswith(")"):
                        cur = None
                    continue
                m = INSERT_RE.match(line)
                if not m:
                    continue
                table = m.group(1).lower()
                if table not in WANTED:
                    continue
                names = [c.strip().strip("`") for c in m.group(3).split(",")] if m.group(3) else cols.get(table)
                if not names:
                    continue
                body = line[m.end():].rstrip().rstrip(";")
                dst = tables.setdefault(table, [])
                for r in sql_rows(body):
                    if len(r) == len(names):
                        dst.append(dict(zip(names, r)))
    return tables


def g(row, *names, default=0):
    """The first present, non-NULL column of several spellings (case-insensitive)."""
    low = row.get("__low")
    if low is None:
        low = {k.lower(): v for k, v in row.items()}
        row["__low"] = low
    for n in names:
        v = low.get(n.lower())
        if v is not None:
            return v
    return default


def num(v, default=0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


# --------------------------------------------------------------------------- factions.txt (converter output)
def load_factions(path):
    """T id faction flags our friendly hostile enemy x4 friend x4 (see net/factions.cpp)."""
    t = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if p[0] != "T" or len(p) < 15:
                continue
            v = [int(x) for x in p[1:15]]
            t[v[0]] = dict(faction=v[1], our=v[3], friendly=v[4], hostile=v[5], enemies=v[6:10], friends=v[10:14])
    return t


def hostile_to(a, b):
    """FactionTemplate::hostileTo of the client (net/factions.cpp)."""
    if b["faction"]:
        if b["faction"] in a["enemies"]:
            return True
        if b["faction"] in a["friends"]:
            return False
    return (a["hostile"] & b["our"]) != 0


def friendly_to(a, b):
    if b["faction"]:
        if b["faction"] in a["enemies"]:
            return False
        if b["faction"] in a["friends"]:
            return True
    return (a["friendly"] & b["our"]) != 0 or (a["our"] & b["our"]) != 0


# --------------------------------------------------------------------------- the pack
def long_s(b, text, limit=4000):
    """A u16-length UTF-8 string past Bin.s's 500 bytes (quest texts), cut on a character boundary."""
    raw = (text or "").encode("utf-8")
    if len(raw) > limit:
        raw = raw[:limit]
        while raw and (raw[-1] & 0xC0) == 0x80:
            raw = raw[:-1]
        if raw and raw[-1] >= 0xC0:
            raw = raw[:-1]
    b.u16(len(raw))
    b.b += raw


def newest_patch(rows, key):
    """vMaNGOS keeps one row per content patch: take the newest one per entry (CMaNGOS has no 'patch')."""
    best = {}
    for r in rows:
        k = int(num(g(r, *key)))
        if not k:
            continue
        p = int(num(g(r, "patch", default=0)))
        if k not in best or p >= best[k][0]:
            best[k] = (p, r)
    return {k: v[1] for k, v in best.items()}


def xp_for(level, rank, ctype, mult):
    if ctype == 8:                       # critters give nothing
        return 0
    base = level * 5 + 45                # vanilla: a kill of the player's own level
    if rank in (1, 2, 3):
        base *= 2                        # elites twice
    return int(base * (mult if mult > 0 else 1.0))


# ---------------------------------------------------------------- WoWVita: creature behaviour
# EventAI (creature_ai_scripts) as CMaNGOS runs it: the events and actions the offline core implements are kept,
# the rest are dropped (an event without one kept action goes).
AI_EVENTS = {0, 1, 2, 3, 4, 5, 6, 7, 9, 11, 12, 13, 14, 21, 27}
AI_ACTIONS = {1, 5, 10, 11, 21, 22, 23, 24, 25, 30, 31, 39, 57}
SPELL_KIND = dict(melee=0, damage=1, heal=2, dot=3, buff=4, debuff=8)


def load_spell_info(path):
    """The converter's spells.txt: '@cast id ms', '@range id min max' and (newer) '@idx cast|dur|range idx a [b]'."""
    info = dict(cast={}, range={}, idx_cast={}, idx_dur={}, idx_range={})
    if not path:
        return info
    with open_text(path) as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            try:
                if p[0] == "@cast":
                    info["cast"][int(p[1])] = int(p[2])
                elif p[0] == "@range":
                    info["range"][int(p[1])] = (float(p[2]), float(p[3]))
                elif p[0] == "@idx" and p[1] == "cast":
                    info["idx_cast"][int(p[2])] = int(p[3])
                elif p[0] == "@idx" and p[1] == "dur":
                    info["idx_dur"][int(p[2])] = int(p[3])
                elif p[0] == "@idx" and p[1] == "range":
                    info["idx_range"][int(p[2])] = (float(p[3]), float(p[4]))
            except (IndexError, ValueError):
                continue
    return info


def creature_spell(s, info, durs):
    """A creature spell of spell_template as the core's SpellDef subset, or None. durs: DurationIndex -> ms."""
    sid = int(num(g(s, "Id")))
    attr, attr1 = int(num(g(s, "Attributes"))), int(num(g(s, "AttributesEx")))
    if attr & 0x40 or attr1 & 0x44:          # passive, channelled
        return None
    eff = [(int(num(g(s, f"Effect{k}"))), int(num(g(s, f"EffectApplyAuraName{k}"))),
            int(num(g(s, f"EffectBasePoints{k}"))), int(num(g(s, f"EffectDieSides{k}"))),
            int(num(g(s, f"EffectAmplitude{k}"))), int(num(g(s, f"EffectImplicitTargetA{k}")))) for k in (1, 2, 3)]
    eff = [e for e in eff if e[0]]
    if not eff:
        return None
    di = int(num(g(s, "DurationIndex")))
    dur = durs.get(di)
    if dur is not None and dur < 0:
        dur = 0                               # -1: until cancelled
    kind = amin = amax = tick = 0
    friendly = False
    e0 = eff[0]
    if e0[0] == 2:                            # school damage
        kind, amin, amax = SPELL_KIND["damage"], e0[2] + 1, e0[2] + max(1, e0[3])
    elif e0[0] in (17, 58, 121, 31):          # weapon damage (+ the creature's own swing, added by the core)
        kind = SPELL_KIND["melee"]
        amin = amax = 0 if e0[0] == 31 else max(0, e0[2] + 1)
        for e in eff[1:]:                     # Heroic Strike style: the bonus may be in a second effect
            if e[0] in (17, 58, 121):
                amin = amax = max(amin, e[2] + 1)
    elif e0[0] == 10:                         # heal
        kind, amin, amax, friendly = SPELL_KIND["heal"], e0[2] + 1, e0[2] + max(1, e0[3]), True
    elif e0[0] in (6, 35, 27) and e0[1] == 3:   # periodic damage
        if not dur or not e0[4]:
            return None
        kind, amin, amax, tick = SPELL_KIND["dot"], e0[2] + 1, e0[2] + max(1, e0[3]), e0[4]
    elif e0[0] in (6, 35, 27):                # any other aura: shown on its target for its duration
        if dur is None or (not dur and e0[5] not in (0, 1, 21, 22, 30, 31, 32, 33)):
            return None                       # (an enemy's aura for ever: left out)
        self_target = e0[5] in (0, 1, 21, 22, 30, 31, 32, 33)   # caster / friendly area around the caster
        kind = SPELL_KIND["buff"] if self_target else SPELL_KIND["debuff"]
        friendly = self_target
    else:
        return None
    ci = int(num(g(s, "CastingTimeIndex")))
    cast = info["cast"].get(sid, info["idx_cast"].get(ci, 0 if ci <= 1 else None))
    if cast is None:
        return None
    ri = int(num(g(s, "RangeIndex")))
    rng = info["range"].get(sid) or info["idx_range"].get(ri)
    if rng is None:
        rng = (0.0, 0.0) if ri in (1, 2) else (0.0, 30.0)   # 1 self, 2 melee reach; else the common 30 yd
    return dict(id=sid, name=str(g(s, "SpellName", default=""))[:60], kind=kind,
                school=int(num(g(s, "School"))), cast=cast, cd=int(num(g(s, "RecoveryTime"))),
                cat=int(num(g(s, "Category"))), catcd=int(num(g(s, "CategoryRecoveryTime"))),
                rmin=rng[0], rmax=rng[1], amin=amin, amax=max(amin, amax), dur=min(dur or 0, 3600000), tick=tick,
                friendly=friendly)


# auras that take control (SPFX): 12 stun, 26 root, 33 slow (its points: -50 = half speed), 7 fear, 27 silence,
# 5 confuse, 25 pacify, 60 pacify + silence
CONTROL_AURAS = {12: 1, 26: 2, 33: 3, 7: 4, 27: 5, 5: 6, 25: 7, 60: 8}


def spell_control(s, durs):
    """(duration ms, [(type, value)]) of the control auras a spell puts on its target, or None."""
    d = durs.get(int(num(g(s, "DurationIndex"))))
    if not d or d <= 0:
        return None
    out = []
    for k in (1, 2, 3):
        if int(num(g(s, f"Effect{k}"))) not in (6, 27, 35):
            continue
        t = CONTROL_AURAS.get(int(num(g(s, f"EffectApplyAuraName{k}"))))
        if t:
            out.append((t, int(num(g(s, f"EffectBasePoints{k}"))) + 1))
    return (min(d, 3600000), out) if out else None


def behaviour_sections(tables, templates, spawn_rows, spawn_moves, used, sec, report, spell_info=None):
    """'MOVE' (random wander and waypoint paths of the spawns), 'CRAI' (walk speed, civilians, EventAI) and
    'AITX' (the texts EventAI says), plus the creature spells EventAI casts appended to 'SPEL'."""
    info = spell_info or load_spell_info(None)
    # -- waypoints: creature_movement (by spawn guid), else creature_movement_template (by entry, path 0)
    by_guid, by_entry = {}, {}
    for r in tables.get("creature_movement", []):
        by_guid.setdefault(int(num(g(r, "Id", "id"))), []).append(r)
    for r in tables.get("creature_movement_template", []):
        if int(num(g(r, "PathId", default=0))) == 0:
            by_entry.setdefault(int(num(g(r, "Entry", "entry"))), []).append(r)
    paths, path_of, moves = [], {}, []
    counts = dict(random=0, waypoint=0, waypoint_no_path=0, points=0)
    for idx, ((entry, *_), (guid, mtype, dist)) in enumerate(zip(spawn_rows, spawn_moves)):
        if mtype == 1 and dist >= 0.5:
            moves.append((idx, 1, min(dist, 60.0), 0xFFFFFFFF))
            counts["random"] += 1
        elif mtype == 2:
            key = ("g", guid) if guid in by_guid else (("e", entry) if entry in by_entry else None)
            if key is None:
                counts["waypoint_no_path"] += 1
                continue
            if key not in path_of:
                rows = sorted(by_guid[guid] if key[0] == "g" else by_entry[entry], key=lambda r: int(num(g(r, "Point"))))
                pts = [(num(g(r, "PositionX")), num(g(r, "PositionY")), num(g(r, "PositionZ")),
                        num(g(r, "Orientation", default=100)), max(0, int(num(g(r, "WaitTime"))))) for r in rows]
                if len(pts) < 2:
                    continue
                path_of[key] = len(paths)
                paths.append(pts[:1000])
                counts["points"] += len(paths[-1])
            moves.append((idx, 2, 0.0, path_of[key]))
            counts["waypoint"] += 1
    mb = Bin()
    mb.u32(len(paths))
    for pts in paths:
        mb.u16(len(pts))
        for (x, y, z, o, w) in pts:
            mb.f32(x); mb.f32(y); mb.f32(z); mb.f32(o); mb.u32(min(w, 0x7FFFFFFF))
    mb.u32(len(moves))
    for (idx, t, d, pi) in moves:
        mb.u32(idx); mb.u8(t); mb.f32(d); mb.u32(pi)
    sec["MOVE"] = mb.b
    counts["paths"] = len(paths)
    report["counts_movement"] = counts

    # -- EventAI
    used_set = set(used)
    spells_db = {int(num(g(r, "Id"))): r for r in tables.get("spell_template", [])}
    durs = dict(info["idx_dur"])
    if not durs:   # an older spells.txt: only the rows of SpellDuration.dbc the food needs are known
        durs = {1: 10000, 8: 15000, 85: 18000, 86: 21000, 106: 24000, 205: 27000, 9: 30000, 42: 3600000, 21: -1}
    by_cr = {}
    for r in tables.get("creature_ai_scripts", []):
        cid = int(num(g(r, "creature_id")))
        if cid in used_set:
            by_cr.setdefault(cid, []).append(r)
    texts_needed, spells_needed, cspells = set(), set(), {}
    ai = {}
    ev_kept = ev_dropped = act_dropped = 0
    for cid, rows in by_cr.items():
        evs = []
        for r in sorted(rows, key=lambda r: int(num(g(r, "id")))):
            et, fl = int(num(g(r, "event_type"))), int(num(g(r, "event_flags")))
            if et not in AI_EVENTS or fl & 0x80:
                ev_dropped += 1
                continue
            acts = []
            for k in (1, 2, 3):
                at = int(num(g(r, f"action{k}_type")))
                ap = [int(num(g(r, f"action{k}_param{j}"))) for j in (1, 2, 3)]
                if at == 11:
                    sid = ap[0]
                    if sid not in cspells:
                        s = spells_db.get(sid)
                        cspells[sid] = creature_spell(s, info, durs) if s else None
                    if not cspells[sid]:
                        at = 0
                if at and at not in AI_ACTIONS:
                    at = 0
                if at == 0 and int(num(g(r, f"action{k}_type"))):
                    act_dropped += 1
                if at == 1:
                    texts_needed.update(t for t in ap if t)
                acts.append((at, ap))
            if not any(a[0] for a in acts):
                ev_dropped += 1
                continue
            evs.append((et, int(num(g(r, "event_inverse_phase_mask"))), int(num(g(r, "event_chance", default=100))), fl,
                        [int(num(g(r, f"event_param{j}"))) for j in range(1, 7)], acts))
            ev_kept += 1
        if evs:
            ai[cid] = evs[:48]
    # -- per creature: walk speed, civilian, the radius it calls for help in, its events
    cb = Bin()
    cb.u32(len(used))
    for entry in used:
        t = templates[entry]
        walk = (num(g(t, "SpeedWalk", "speed_walk", default=1.0)) or 1.0) * 2.5
        flags = (1 if int(num(g(t, "Civilian", default=0))) else 0) | (2 if entry in ai else 0)
        cb.u32(entry); cb.f32(walk); cb.f32(num(g(t, "CallForHelp", default=0))); cb.u8(flags)
        cb.f32(num(g(t, "Leash", default=0))); cb.u32(int(num(g(t, "Pursuit", default=15000))) or 15000)
        evs = ai.get(entry, [])
        cb.u8(len(evs))
        for (et, mask, chance, fl, ep, acts) in evs:
            cb.u8(et); cb.u32(mask); cb.u8(max(0, min(100, chance))); cb.u16(fl)
            for v in ep:
                cb.i32(v)
            for (at, ap) in acts:
                cb.u8(at)
                for v in ap:
                    cb.i32(v)
    sec["CRAI"] = cb.b
    # -- texts: broadcast_text (positive ids) and creature_ai_texts (negative ids)
    bt = {int(num(g(r, "Id"))): r for r in tables.get("broadcast_text", [])}
    at_ = {int(num(g(r, "entry"))): r for r in tables.get("creature_ai_texts", [])}
    tx = Bin()
    out = []
    for tid in sorted(texts_needed):
        if tid > 0 and tid in bt:
            r = bt[tid]
            text = str(g(r, "Text", default="") or g(r, "Text1", default="") or "")
            out.append((tid, int(num(g(r, "ChatTypeID"))), int(num(g(r, "EmoteID1"))), text))
        elif tid < 0 and tid in at_:
            r = at_[tid]
            bid = int(num(g(r, "broadcast_text_id")))
            text = str(g(r, "content_default", default="") or "")
            if bid in bt:
                text = str(g(bt[bid], "Text", default="") or text)
            out.append((tid, int(num(g(r, "type"))), int(num(g(r, "emote"))), text))
    tx.u32(len(out))
    for (tid, ctype, emote, text) in out:
        tx.i32(tid); tx.u8(ctype); tx.u32(emote); long_s(tx, text, 500)
    sec["AITX"] = tx.b
    # -- the creature spells EventAI casts, after the sample's spells
    old = sec["SPEL"]
    n_old = int.from_bytes(old[:4], "little")
    sb = Bin()
    have = set(sample_spell_ids(old))
    add = [cs for sid, cs in sorted(cspells.items()) if cs and sid not in have]
    sb.b += (n_old + len(add)).to_bytes(4, "little") + old[4:]
    for c in add:
        sb.u32(c["id"]); sb.s(c["name"]); sb.u8(c["kind"]); sb.u8(c["school"]); sb.u32(c["cast"]); sb.u32(c["cd"])
        sb.u32(c["cat"]); sb.u32(c["catcd"]); sb.f32(c["rmin"]); sb.f32(c["rmax"]); sb.u8(0); sb.u32(0)
        sb.i32(c["amin"]); sb.i32(c["amax"]); sb.u32(c["dur"]); sb.u32(c["tick"]); sb.i32(0); sb.f32(0.0)
        sb.u8(1 if c["friendly"] else 0); sb.u8(0)
    sec["SPEL"] = sb.b
    # -- what the auras of those spells do (stun, root, slow, fear, silence, confuse, pacify)
    fx = []
    for c in add:
        ctl = spell_control(spells_db[c["id"]], durs)
        if ctl:
            fx.append((c["id"],) + ctl)
    xb = Bin()
    xb.u32(len(fx))
    for (sid, dur, effs) in fx:
        xb.u32(sid); xb.u32(dur); xb.u8(len(effs))
        for (t, v) in effs:
            xb.u8(t); xb.i32(v)
    sec["SPFX"] = xb.b
    kinds = {}
    for c in add:
        kinds[c["kind"]] = kinds.get(c["kind"], 0) + 1
    report["counts_eventai"] = dict(creatures=len(ai), events=ev_kept, events_dropped=ev_dropped,
                                    actions_dropped=act_dropped, texts=len(out), spells=len(add),
                                    spells_not_supported=sum(1 for c in cspells.values() if not c),
                                    spell_kinds=kinds, control_spells=len(fx), durations_known=len(info["idx_dur"]) > 0)


def group_sections(tables, templates, spawn_rows, spawn_moves, map_id, sec, report):
    """'POOL': spawns of which only a few are up at a time (pool_creature / pool_creature_template max_limit,
    spawn_group MaxCount - the rare spawns, Hogger); 'LINK': creature_linking (by spawn) and creature_linking_template
    (by entry, the nearest master within search_range): 0x1 aggro on the master's aggro, 0x2 the master too,
    0x200 follow the master, 0x4000 evade with it."""
    idx_of = {guid: i for i, (guid, _t, _d) in enumerate(spawn_moves)}
    pools = {}   # key -> (limit, [spawn index])
    limit_of = {int(num(g(r, "entry"))): int(num(g(r, "max_limit"))) for r in tables.get("pool_template", [])}
    for r in tables.get("pool_creature", []):
        i = idx_of.get(int(num(g(r, "guid"))))
        if i is not None:
            pe = int(num(g(r, "pool_entry")))
            pools.setdefault(("p", pe), [limit_of.get(pe, 0), []])[1].append(i)
    by_entry = {}
    for i, row in enumerate(spawn_rows):
        by_entry.setdefault(row[0], []).append(i)
    for r in tables.get("pool_creature_template", []):
        pe = int(num(g(r, "pool_entry")))
        for i in by_entry.get(int(num(g(r, "id"))), []):
            pools.setdefault(("p", pe), [limit_of.get(pe, 0), []])[1].append(i)
    sg_max = {int(num(g(r, "Id"))): int(num(g(r, "MaxCount"))) for r in tables.get("spawn_group", [])
              if int(num(g(r, "Type"))) == 0}
    for r in tables.get("spawn_group_spawn", []):
        gid = int(num(g(r, "Id")))
        i = idx_of.get(int(num(g(r, "Guid"))))
        if i is not None and sg_max.get(gid, 0) > 0:
            pools.setdefault(("g", gid), [sg_max[gid], []])[1].append(i)
    seen, out = set(), []
    for key in sorted(pools, key=str):
        lim, mem = pools[key]
        mem = sorted(set(m for m in mem if m not in seen))
        if lim <= 0 or len(mem) <= lim:
            continue
        seen.update(mem)
        out.append((lim, mem))
    pb = Bin()
    pb.u32(len(out))
    for lim, mem in out:
        pb.u16(lim); pb.u16(len(mem))
        for m in mem:
            pb.u32(m)
    sec["POOL"] = pb.b
    # linking
    links = {}
    for r in tables.get("creature_linking", []):
        sl, ma = idx_of.get(int(num(g(r, "guid")))), idx_of.get(int(num(g(r, "master_guid"))))
        if sl is not None and ma is not None and sl != ma:
            links[sl] = (ma, int(num(g(r, "flag"))))
    for r in tables.get("creature_linking_template", []):
        if int(num(g(r, "map"))) != map_id:
            continue
        rng_ = num(g(r, "search_range"))
        masters = by_entry.get(int(num(g(r, "master_entry"))), [])
        if not masters:
            continue
        for sl in by_entry.get(int(num(g(r, "entry"))), []):
            if sl in links:
                continue
            sx, sy = spawn_rows[sl][1], spawn_rows[sl][2]
            best = min(masters, key=lambda m: (spawn_rows[m][1] - sx) ** 2 + (spawn_rows[m][2] - sy) ** 2)
            d = ((spawn_rows[best][1] - sx) ** 2 + (spawn_rows[best][2] - sy) ** 2) ** 0.5
            if best != sl and (rng_ <= 0 or d <= rng_):
                links[sl] = (best, int(num(g(r, "flag"))))
    lb = Bin()
    keep = [(sl, ma, fl) for sl, (ma, fl) in sorted(links.items()) if fl & 0x4203]
    lb.u32(len(keep))
    for sl, ma, fl in keep:
        lb.u32(sl); lb.u32(ma); lb.u16(fl & 0xFFFF)
    sec["LINK"] = lb.b
    report["counts_groups"] = dict(pools=len(out), pooled_spawns=sum(len(m) for _, m in out), links=len(keep),
                                   followers=sum(1 for k in keep if k[2] & 0x200))


def sample_spell_ids(spel):
    """The spell ids of a SPEL section (to skip ones the sample already defines)."""
    import struct
    ids, pos = [], 4
    n = int.from_bytes(spel[:4], "little")
    for _ in range(n):
        ids.append(struct.unpack_from("<I", spel, pos)[0]); pos += 4
        ln = struct.unpack_from("<H", spel, pos)[0]; pos += 2 + ln
        pos += 1 + 1 + 4 * 4 + 4 + 4 + 1 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 1 + 1
    return ids


def start_outfits(path, items_db, report):
    """[(race, class, sex, [item])]: glue.txt 'S' lines (a newer converter: CharStartOutfit.dbc's items), else its 'O'
    lines (only what is shown on the character: display id + inventory type) matched to item_template - a level 1-5
    item of that look and slot the race and class may use."""
    if not path:
        return []
    s_lines, o_lines = [], []
    with open_text(path) as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            try:
                if p[0] == "S" and len(p) > 4:
                    s_lines.append((int(p[1]), int(p[2]), int(p[3]), [int(x) for x in p[4:] if x]))
                elif p[0] == "O" and len(p) > 5:
                    o_lines.append((int(p[1]), int(p[2]), int(p[3]), [(int(p[k]), int(p[k + 1])) for k in range(4, len(p) - 1, 2)]))
            except ValueError:
                continue
    if s_lines:
        out = [(r, c, x, [i for i in its if i in items_db]) for (r, c, x, its) in s_lines]
        report["counts_outfits"] = dict(source="S lines", outfits=len(out))
        return out
    # (the slots of an older glue.txt's O lines can be off by one: the look alone is matched)
    by_look = {}
    for e, r in items_db.items():
        if int(num(g(r, "ItemLevel"))) <= 5:
            by_look.setdefault(int(num(g(r, "displayid"))), []).append(e)
    out, missing = [], 0
    for (race, cls, sex, looks) in o_lines:
        its = []
        for (disp, _inv) in looks:
            cand = []
            for e in by_look.get(disp, []):
                r = items_db[e]
                ac, ar = int(num(g(r, "AllowableClass", default=-1))), int(num(g(r, "AllowableRace", default=-1)))
                ok_c = ac in (-1, 0) or ac & (1 << (cls - 1))
                ok_r = ar in (-1, 0) or ar & (1 << (race - 1))
                cand.append((0 if ok_c and ok_r else 1, e))
            if cand:
                its.append(min(cand)[1])
            else:
                missing += 1
        out.append((race, cls, sex, its))
    report["counts_outfits"] = dict(source="O lines (shown items only)", outfits=len(out), not_found=missing)
    return out


def build(tables, factions, map_id, report, player_faction=1, spell_info=None, glue_path=None):
    sample = build_sample()
    templates = newest_patch(tables.get("creature_template", []), ("Entry", "entry"))
    items_db = newest_patch(tables.get("item_template", []), ("entry",))
    models = {int(num(g(r, "modelid", "display_id"))): r for r in tables.get("creature_model_info", [])}

    # ---- spawns of the map (vMaNGOS: patch_min/patch_max; only those of the last patch)
    spawn_rows = []
    spawn_moves = []   # WoWVita: (guid, MovementType, spawndist) of each spawn row, in the same order
    skipped_invisible = 0
    spawn_entry = {}   # CMaNGOS: id 0 = one of several entries (creature_spawn_entry); the spawn guid picks one
    for r in tables.get("creature_spawn_entry", []):
        spawn_entry.setdefault(int(num(g(r, "guid"))), []).append(int(num(g(r, "entry"))))
    # WoWVita: a spawn of a spawn_group with no entry of its own takes one of the group's (spawn_group_entry: those with a
    # Chance get it in percent, the rest share what is left) - picked once per spawn, by its guid
    group_of = {int(num(g(r, "Guid"))): int(num(g(r, "Id"))) for r in tables.get("spawn_group_spawn", [])}
    group_entries = {}
    for r in tables.get("spawn_group_entry", []):
        group_entries.setdefault(int(num(g(r, "Id"))), []).append((int(num(g(r, "Entry"))), int(num(g(r, "Chance")))))

    def group_pick(guid):
        ents = group_entries.get(group_of.get(guid, -1))
        if not ents:
            return 0
        fixed = sum(c for _, c in ents if c > 0)
        rest = [e for e, c in ents if c <= 0]
        share = max(0.0, 100.0 - fixed) / len(rest) if rest else 0.0
        roll = (guid * 2654435761 % 2 ** 32) / 2 ** 32 * max(100.0, fixed + share * len(rest))
        for e, c in ents:
            roll -= c if c > 0 else share
            if roll < 0:
                return e
        return ents[-1][0]
    for r in tables.get("creature", []):
        if int(num(g(r, "map"))) != map_id:
            continue
        pmax = g(r, "patch_max", default=None)
        if pmax is not None and int(num(pmax)) < 10:
            continue
        entry = int(num(g(r, "id", "id1", "entry")))
        if not entry:
            choices = sorted(spawn_entry.get(int(num(g(r, "guid"))), []))
            if choices:
                entry = choices[int(num(g(r, "guid"))) % len(choices)]
            else:
                entry = group_pick(int(num(g(r, "guid"))))
        t = templates.get(entry)
        if not t:
            report["warn"].append(f"spawn of unknown creature {entry}")
            continue
        unit_flags = int(num(g(t, "UnitFlags", "unit_flags")))
        extra = int(num(g(t, "ExtraFlags", "flags_extra")))
        name = str(g(t, "Name", "name", default=""))
        display = [int(num(g(t, f"ModelId{i}", f"display_id{i}", f"modelid_{i}"))) for i in (1, 2, 3, 4)]
        if (not any(display) or unit_flags & 0x02000000 or extra & 0x80
                or "trigger" in name.lower() or "(only gm" in name.lower() or name.startswith("[")):
            skipped_invisible += 1
            continue
        respawn = int(num(g(r, "spawntimesecsmin", "spawntimesecs", "spawntimesecsmax", default=300)))
        spawn_rows.append((entry, num(g(r, "position_x")), num(g(r, "position_y")), num(g(r, "position_z")),
                           num(g(r, "orientation")), max(5000, respawn * 1000)))
        spawn_moves.append((int(num(g(r, "guid"))), int(num(g(r, "MovementType", "movement_type"))),
                            num(g(r, "spawndist", "wander_distance"))))
    used = sorted({s[0] for s in spawn_rows})

    # ---- creature templates
    crea, loot_ids, money_by_loot, factions_used = [], {}, {}, set()
    for entry in used:
        t = templates[entry]
        mn = int(num(g(t, "MinLevel", "level_min", "minlevel", default=1))) or 1
        mx = int(num(g(t, "MaxLevel", "level_max", "maxlevel", default=mn))) or mn
        hp = int(num(g(t, "MinLevelHealth", "health_min", "minhealth")))
        if hp <= 0:
            hp = max(20, mn * 25)
        dm = num(g(t, "DamageMultiplier", "damage_multiplier", default=1.0)) or 1.0
        dmin = int(num(g(t, "MinMeleeDmg", "dmg_min", "mindmg", default=2)) * dm)
        dmax = int(num(g(t, "MaxMeleeDmg", "dmg_max", "maxdmg", default=4)) * dm)
        mana = int(num(g(t, "MinLevelMana", "mana_min", "minmana")))
        rank = int(num(g(t, "Rank", "rank")))
        ctype = int(num(g(t, "CreatureType", "type")))
        faction = int(num(g(t, "Faction", "FactionAlliance", "faction", "faction_a")))
        display = [int(num(g(t, f"ModelId{i}", f"display_id{i}", f"modelid_{i}"))) for i in (1, 2, 3, 4)]
        disp = next(d for d in display if d)
        mi = models.get(disp, {})
        loot = int(num(g(t, "LootId", "loot_id", "lootid")))
        gold = (int(num(g(t, "MinLootGold", "gold_min", "mingold"))), int(num(g(t, "MaxLootGold", "gold_max", "maxgold"))))
        if not loot and gold[1] > 0:
            loot = entry            # money only: a table of its own
        if loot:
            loot_ids[loot] = True
            money_by_loot.setdefault(loot, gold)
        factions_used.add(faction)
        scale = num(g(t, "Scale", "scale", default=1.0)) or 1.0
        crea.append(dict(
            entry=entry, name=str(g(t, "Name", "name", default=f"Creature {entry}")),
            sub=str(g(t, "SubName", "subname", default="") or ""), display=disp, mn=mn, mx=mx, faction=faction,
            hp=max(1, hp), armor=int(num(g(t, "Armor", "armor"))),
            xp=xp_for(mn, rank, ctype, num(g(t, "ExperienceMultiplier", "xp_multiplier", default=1.0))),
            dmin=max(1, min(65535, dmin)), dmax=max(1, min(65535, max(dmin, dmax))),
            atk=max(500, int(num(g(t, "MeleeBaseAttackTime", "base_attack_time", "baseattacktime", default=2000)))),
            pt=0, power=max(0, mana), rank=min(4, rank), ctype=min(255, ctype),
            npc=int(num(g(t, "NpcFlags", "npc_flags", "npcflag"))), scale=scale,
            speed=(num(g(t, "SpeedRun", "speed_run", default=1.14286)) or 1.14286) * 7.0,
            aggro=min(30.0, num(g(t, "Detection", default=18.0)) or 18.0) if faction else 0.0,
            reach=num(g(mi, "combat_reach", default=1.5)) or 1.5,
            bound=num(g(mi, "bounding_radius", default=0.5)) or 0.5,
            loot=loot))

    # ---- loot: one level of reference tables expanded, quest drops left out, max 64 entries per table
    by_entry = {}
    for r in tables.get("creature_loot_template", []):
        by_entry.setdefault(int(num(g(r, "entry"))), []).append(r)
    refs = {}
    for r in tables.get("reference_loot_template", []):
        refs.setdefault(int(num(g(r, "entry"))), []).append(r)

    quest_drops = []

    def entries_of(rows, scale=1.0, depth=0):
        out = []
        groups = {}
        for r in rows:
            groups.setdefault(int(num(g(r, "groupid"))), []).append(r)
        for gid, rs in groups.items():
            explicit = sum(max(0.0, num(g(r, "ChanceOrQuestChance", "chance"))) for r in rs)
            zero = [r for r in rs if num(g(r, "ChanceOrQuestChance", "chance")) == 0]
            share = max(0.0, 100.0 - explicit) / len(zero) if (gid and zero) else 100.0
            for r in rs:
                chance = num(g(r, "ChanceOrQuestChance", "chance"))
                if chance < 0:                                 # a quest drop: only while a quest wants it (QLOT)
                    if depth == 0 and int(num(g(r, "mincountOrRef", "mincount", default=1))) > 0:
                        quest_drops.append((int(num(g(r, "item"))), -chance * scale))
                    continue
                if chance == 0:
                    chance = share
                mnc = int(num(g(r, "mincountOrRef", "mincount", default=1)))
                if mnc < 0:
                    if depth < 2 and -mnc in refs:
                        out += entries_of(refs[-mnc], scale * chance / 100.0, depth + 1)
                    continue
                out.append((int(num(g(r, "item"))), chance * scale, max(1, mnc),
                            max(1, mnc, int(num(g(r, "maxcount", default=1))))))
        return out

    loot_tables, item_needed = [], set()
    qloot = {}
    for lid in sorted(loot_ids):
        quest_drops = []
        ents = [e for e in entries_of(by_entry.get(lid, [])) if e[0] in items_db]
        if quest_drops:
            qloot[lid] = [(i, c) for i, c in quest_drops if i in items_db][:16]
        ents.sort(key=lambda e: -e[1])
        ents = ents[:64]
        for e in ents:
            item_needed.add(e[0])
        lo, hi = money_by_loot.get(lid, (0, 0))
        loot_tables.append((lid, lo, max(lo, hi), ents))

    # ---- vendors: npc_vendor by creature entry, plus CMaNGOS npc_vendor_template through VendorTemplateId
    direct, tmpl = {}, {}
    for r in tables.get("npc_vendor", []):
        direct.setdefault(int(num(g(r, "entry"))), []).append(r)
    for r in tables.get("npc_vendor_template", []):
        tmpl.setdefault(int(num(g(r, "entry"))), []).append(r)
    vendors = {}
    for t in crea:
        if not (t["npc"] & 0x4):
            continue
        rows = list(direct.get(t["entry"], []))
        vt = int(num(g(templates[t["entry"]], "VendorTemplateId", "vendor_id")))
        if vt:
            rows += tmpl.get(vt, [])
        goods, seen = [], set()
        for r in sorted(rows, key=lambda r: int(num(g(r, "slot", default=0)))):
            item = int(num(g(r, "item")))
            if item in seen or item not in items_db:
                continue
            if int(num(g(r, "maxcount"))) > 0:
                continue                 # limited-stock rarities: left out (no restock timers offline)
            seen.add(item)
            goods.append(item)
        if goods:
            vendors[t["entry"]] = goods[:255]
            item_needed.update(goods)

    # ---- game objects of the map: templates, spawns, chest loot (quest drops flagged)
    go_t = {int(num(g(r, "entry"))): r for r in tables.get("gameobject_template", [])}
    go_spawn_entry = {}
    for r in tables.get("gameobject_spawn_entry", []):
        go_spawn_entry.setdefault(int(num(g(r, "guid"))), []).append(int(num(g(r, "entry"))))
    go_spawns = []
    for r in tables.get("gameobject", []):
        if int(num(g(r, "map"))) != map_id:
            continue
        gid = int(num(g(r, "id")))
        if not gid:
            ch = sorted(go_spawn_entry.get(int(num(g(r, "guid"))), []))
            gid = ch[int(num(g(r, "guid"))) % len(ch)] if ch else 0
        t = go_t.get(gid)
        if not t or int(num(g(t, "type"))) not in GO_TYPES or not int(num(g(t, "displayId"))):
            continue
        go_spawns.append((gid, num(g(r, "position_x")), num(g(r, "position_y")), num(g(r, "position_z")),
                          num(g(r, "orientation")), [num(g(r, f"rotation{i}")) for i in range(4)],
                          max(5000, int(num(g(r, "spawntimesecsmin", "spawntimesecs", default=300))) * 1000)))
    go_used = sorted({s_[0] for s_ in go_spawns})
    go_loot_rows = {}
    for r in tables.get("gameobject_loot_template", []):
        go_loot_rows.setdefault(int(num(g(r, "entry"))), []).append(r)
    go_loot = {}            # loot id -> (min gold, max gold, [(item, chance, min, max, quest only)])
    for e in go_used:
        t = go_t[e]
        if int(num(g(t, "type"))) != 3:
            continue
        lid = int(num(g(t, "data1")))
        if not lid or lid in go_loot:
            continue
        quest_drops = []
        ents = [(i, c, mn, mx, 0) for (i, c, mn, mx) in entries_of(go_loot_rows.get(lid, [])) if i in items_db]
        ents += [(i, c, 1, 1, 1) for (i, c) in quest_drops if i in items_db]
        ents.sort(key=lambda x: (-x[4], -x[1]))
        go_loot[lid] = (int(num(g(t, "mingold"))), int(num(g(t, "maxgold"))), ents[:16])
        for x in ents[:16]:
            item_needed.add(x[0])
    go_spawned = set(go_used)

    # ---- quests: given and taken by creatures of this map; only kinds the offline core can finish
    spawned = {t["entry"] for t in crea}
    starters, enders = {}, {}
    for r in tables.get("creature_questrelation", []):
        c = int(num(g(r, "id")))
        if c in spawned:
            starters.setdefault(int(num(g(r, "quest"))), []).append(c)
    for r in tables.get("creature_involvedrelation", []):
        c = int(num(g(r, "id")))
        if c in spawned:
            enders.setdefault(int(num(g(r, "quest"))), []).append(c)
    go_starters, go_enders = {}, {}       # quest givers that are objects (wanted posters, books)
    for r in tables.get("gameobject_questrelation", []):
        c = int(num(g(r, "id")))
        if c in go_spawned:
            go_starters.setdefault(int(num(g(r, "quest"))), []).append(c)
    for r in tables.get("gameobject_involvedrelation", []):
        c = int(num(g(r, "id")))
        if c in go_spawned:
            go_enders.setdefault(int(num(g(r, "quest"))), []).append(c)
    obtainable = set()                       # items a player can get here: creature loot, quest drops, vendors
    for lid, lo, hi, ents in loot_tables:
        obtainable.update(e[0] for e in ents)
    for lid, drops in qloot.items():
        obtainable.update(i for i, _ in drops)
    for goods in vendors.values():
        obtainable.update(goods)
    for _lo, _hi, ents in go_loot.values():
        obtainable.update(x[0] for x in ents)
    quests_db = newest_patch(tables.get("quest_template", []), ("entry",))
    for qid, q in quests_db.items():         # ... and what quests given here hand out (letters, rewards)
        if qid in starters or qid in go_starters:
            for k in ["SrcItemId"] + [f"RewItemId{i}" for i in (1, 2, 3, 4)] + [f"RewChoiceItemId{i}" for i in range(1, 7)]:
                v = int(num(g(q, k)))
                if v:
                    obtainable.add(v)
    quests, skipped_q = [], {}
    # exploration quests: reaching an area trigger completes them (areatrigger_involvedrelation; the trigger's shape is
    # the client's AreaTrigger.dbc, which reports it with CMSG_AREATRIGGER)
    quest_triggers = {}
    for r in tables.get("areatrigger_involvedrelation", []):
        quest_triggers.setdefault(int(num(g(r, "quest"))), []).append(int(num(g(r, "id"))))

    def skip(why):
        skipped_q[why] = skipped_q.get(why, 0) + 1

    for qid in sorted(quests_db):
        q = quests_db[qid]
        if (qid not in starters and qid not in go_starters) or (qid not in enders and qid not in go_enders):
            continue                         # not given and taken on this map
        if (int(num(g(q, "SpecialFlags"))) & 2 or int(num(g(q, "QuestFlags"))) & 4) and qid not in quest_triggers:
            skip("scripted event"); continue
        if int(num(g(q, "LimitTime"))) > 0:
            skip("timed"); continue
        if int(num(g(q, "RequiredSkill"))) or int(num(g(q, "RepObjectiveFaction"))):
            skip("profession / reputation"); continue
        if any(int(num(g(q, f"ReqSpellCast{i}"))) for i in (1, 2, 3, 4)):
            skip("cast a spell"); continue
        objs, bad = [], False
        for i in (1, 2, 3, 4):
            c = int(num(g(q, f"ReqCreatureOrGOId{i}")))
            n = int(num(g(q, f"ReqCreatureOrGOCount{i}")))
            if c < 0 and n and -c not in go_spawned:
                bad = "use a game object not on this map"
            elif c > 0 and n and c not in spawned:
                bad = "kill a creature not on this map"
            objs.append((c & 0xFFFFFFFF if n else 0, n if c else 0))   # (a game object: -entry, as the wire has it)
        src = int(num(g(q, "SrcItemId")))
        items_req = []
        for i in (1, 2, 3, 4):
            it = int(num(g(q, f"ReqItemId{i}")))
            n = int(num(g(q, f"ReqItemCount{i}")))
            if it and n and it not in obtainable and it != src:
                bad = bad or "an item that cannot be got here"
            items_req.append((it if n else 0, n if it else 0))
        if bad:
            skip(bad); continue
        rew = [(int(num(g(q, f"RewItemId{i}"))), int(num(g(q, f"RewItemCount{i}")))) for i in (1, 2, 3, 4)]
        cho = [(int(num(g(q, f"RewChoiceItemId{i}"))), int(num(g(q, f"RewChoiceItemCount{i}")))) for i in range(1, 7)]
        rew = [(i, n) if i in items_db and n else (0, 0) for i, n in rew]
        cho = [(i, n) if i in items_db and n else (0, 0) for i, n in cho]
        for i, n in rew + cho + items_req + [(src, 1)]:
            if i:
                item_needed.add(i)
        quests.append(dict(
            id=qid, level=int(num(g(q, "QuestLevel", default=1))), minlevel=int(num(g(q, "MinLevel"))),
            races=int(num(g(q, "RequiredRaces"))), classes=int(num(g(q, "RequiredClasses"))),
            zone=int(num(g(q, "ZoneOrSort"))), type=int(num(g(q, "Type"))), flags=int(num(g(q, "QuestFlags"))),
            special=int(num(g(q, "SpecialFlags"))), prev=int(num(g(q, "PrevQuestId"))),
            next=int(num(g(q, "NextQuestInChain"))), src=src, srccount=int(num(g(q, "SrcItemCount", default=1))) or 1,
            money=int(num(g(q, "RewOrReqMoney"))), moneymax=int(num(g(q, "RewMoneyMaxLevel"))),
            texts=[str(g(q, k, default="") or "") for k in ("Title", "Details", "Objectives", "OfferRewardText",
                                                             "RequestItemsText", "EndText")],
            objtext=[str(g(q, f"ObjectiveText{i}", default="") or "") for i in (1, 2, 3, 4)],
            kills=objs, items=items_req, rew=rew, cho=cho,
            starters=sorted(set(starters.get(qid, []))), enders=sorted(set(enders.get(qid, []))),
            go_starters=sorted(set(go_starters.get(qid, []))), go_enders=sorted(set(go_enders.get(qid, [])))))
    kept = {q["id"] for q in quests}
    for q in quests:                         # a step left out of a chain must not lock the rest
        if q["prev"] and abs(q["prev"]) not in kept:
            q["prev"] = 0

    # ---- what NPCs say first (gossip_menu -> npc_text)
    menus = {}
    for r in tables.get("gossip_menu", []):
        menus.setdefault(int(num(g(r, "entry"))), int(num(g(r, "text_id"))))
    texts = {int(num(g(r, "ID"))): r for r in tables.get("npc_text", [])}
    greetings = {}
    for t in crea:
        if not t["npc"]:
            continue
        mid = int(num(g(templates[t["entry"]], "GossipMenuId", "gossip_menu_id")))
        tr = texts.get(menus.get(mid, -1))
        if tr:
            txt = str(g(tr, "text0_0", default="") or "") or str(g(tr, "text0_1", default="") or "")
            if txt:
                greetings[t["entry"]] = txt[:600]

    # ---- WoWVita "OUTF": what a new character starts with (the converter's glue.txt: CharStartOutfit.dbc)
    sec_extra = {}
    outfits = start_outfits(glue_path, items_db, report)
    for (_r, _c, _s, its) in outfits:
        item_needed.update(its)
    ob = Bin()
    ob.u32(len(outfits))
    for (race, cls, sex, its) in outfits:
        ob.u8(race); ob.u8(cls); ob.u8(sex); ob.u8(min(len(its), 24))
        for it in its[:24]:
            ob.u32(it)
    sec_extra["OUTF"] = ob.b

    # ---- items: what the loot and the companions need (the sample's item ids are in the DB too)
    for e in (25, 38, 2589, 750, 6948):   # (6948: the Hearthstone every character carries)
        item_needed.add(e)
    items = []
    for entry in sorted(item_needed):
        r = items_db.get(entry)
        if not r:
            continue
        items.append(dict(
            entry=entry, name=str(g(r, "name", default=f"Item {entry}")), desc=str(g(r, "description", default="") or ""),
            display=int(num(g(r, "displayid", "display_id"))), quality=int(num(g(r, "Quality", "quality"))),
            inv=int(num(g(r, "InventoryType", "inventory_type"))), ilvl=int(num(g(r, "ItemLevel", "item_level", default=1))),
            req=int(num(g(r, "RequiredLevel", "required_level"))), stack=max(1, int(num(g(r, "stackable", default=1)))),
            buy=int(num(g(r, "BuyPrice", "buy_price"))), sell=int(num(g(r, "SellPrice", "sell_price"))),
            armor=int(num(g(r, "armor"))), delay=int(num(g(r, "delay"))),
            dmin=num(g(r, "dmg_min1")), dmax=num(g(r, "dmg_max1")),
            cls=int(num(g(r, "class"))), sub=int(num(g(r, "subclass"))),
            buycount=max(1, int(num(g(r, "BuyCount", "buy_count", default=1))))))
    known_items = {i["entry"] for i in items}

    # ---- "ITX1": what an item is besides its basics: a bag's slots, its durability, and what using it does
    # (food 84 / drink 85 auras, healing potions effect 10, mana potions effect 30 - from spell_template = Spell.dbc).
    # SpellDuration.dbc is not in the database: the food durations are its rows (1 10 s, 8 15 s, 85 18 s, 86 21 s,
    # 106 24 s, 205 27 s, 9 30 s, 42 1 h).
    spells = {int(num(g(r, "Id"))): r for r in tables.get("spell_template", [])}
    dur_ms = {1: 10000, 8: 15000, 85: 18000, 86: 21000, 106: 24000, 205: 27000, 9: 30000, 42: 3600000}
    itx = []
    for entry in sorted(known_items):
        r = items_db.get(entry)
        slots = int(num(g(r, "ContainerSlots"))) if int(num(g(r, "class"))) == 1 else 0
        maxdur = int(num(g(r, "MaxDurability")))
        use = (0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        sid = int(num(g(r, "spellid_1")))
        s = spells.get(sid) if int(num(g(r, "spelltrigger_1"))) == 0 else None
        if s:
            hp, mana, kind, dur = [0, 0], [0, 0], 0, 0
            for k in (1, 2, 3):
                eff, aura = int(num(g(s, f"Effect{k}"))), int(num(g(s, f"EffectApplyAuraName{k}")))
                bp, die = int(num(g(s, f"EffectBasePoints{k}"))), int(num(g(s, f"EffectDieSides{k}")))
                if eff == 6 and aura in (84, 85):   # food / drink: the tooltip's "over N sec" = (bp + 1) per 5 s
                    d = dur_ms.get(int(num(g(s, "DurationIndex"))))
                    if not d:
                        continue
                    dur = d
                    tot = (bp + 1) * d // 5000
                    if aura == 84: hp = [tot, tot]; kind |= 1
                    else: mana = [tot, tot]; kind |= 2
                elif eff == 10:                     # heal
                    hp = [bp + 1, bp + max(1, die)]; kind |= 4
                elif eff == 30 and int(num(g(s, f"EffectMiscValue{k}"))) == 0:   # energize: mana
                    mana = [bp + 1, bp + max(1, die)]; kind |= 8
            if kind:
                cd = int(num(g(r, "spellcategorycooldown_1")))
                if cd <= 0: cd = int(num(g(r, "spellcooldown_1")))
                if cd <= 0: cd = int(num(g(s, "CategoryRecoveryTime"))) or int(num(g(s, "RecoveryTime")))
                flags = 1 if int(num(g(s, "Attributes"))) & 0x10000000 else 0   # not in combat (food, drink)
                if int(num(g(r, "spellcharges_1"))) < 0: flags |= 2                # used up
                use = (kind, sid, hp[0], hp[1], mana[0], mana[1], dur, max(0, cd), int(num(g(r, "spellcategory_1"))), flags)
        if slots or maxdur or use[0]:
            itx.append((entry, slots, maxdur) + use)

    # ---- hostility towards the player's faction template (1: Human; the companions use it too)
    host = []
    hostile_n = neutral_n = 0
    me = factions.get(player_faction)
    for f in sorted(factions_used):
        if not f or f == player_faction:
            continue
        ft = factions.get(f)
        if not ft or not me:
            report["warn"].append(f"faction template {f} not in factions.txt")
            continue
        if hostile_to(ft, me) or hostile_to(me, ft):
            host.append((f, player_faction))
            hostile_n += 1
        elif not friendly_to(ft, me) and not friendly_to(me, ft):
            host.append((f | 0x80000000, player_faction))
            neutral_n += 1

    # ---- sections
    sec = {}
    cb = Bin()
    cb.u32(len(crea))
    for t in crea:
        cb.u32(t["entry"]); cb.s(t["name"][:200]); cb.s(t["sub"][:200]); cb.u32(t["display"])
        cb.u16(t["mn"]); cb.u16(t["mx"]); cb.u32(t["faction"]); cb.u32(t["hp"]); cb.u32(t["armor"]); cb.u32(t["xp"])
        cb.u16(t["dmin"]); cb.u16(t["dmax"]); cb.u32(t["atk"]); cb.u8(t["pt"]); cb.u32(t["power"]); cb.u8(t["rank"])
        cb.u8(t["ctype"]); cb.u32(t["npc"]); cb.f32(t["scale"]); cb.f32(t["speed"]); cb.f32(t["aggro"])
        cb.f32(t["reach"]); cb.f32(t["bound"]); cb.u32(t["loot"]); cb.u8(0)
    sec["CREA"] = cb.b

    sp = Bin()
    sp.u32(len(spawn_rows))
    for (entry, x, y, z, o, respawn) in spawn_rows:
        sp.u32(entry); sp.f32(x); sp.f32(y); sp.f32(z); sp.f32(o); sp.u32(respawn); sp.u8(0)
    sec["SPWN"] = sp.b

    lb = Bin()
    lb.u32(len(loot_tables))
    for (lid, lo, hi, ents) in loot_tables:
        ents = [e for e in ents if e[0] in known_items]
        lb.u32(lid); lb.u32(lo); lb.u32(hi); lb.u16(len(ents))
        for (item, chance, mnc, mxc) in ents:
            lb.u32(item); lb.u16(max(1, min(10000, int(round(chance * 100))))); lb.u16(min(65535, mnc)); lb.u16(min(65535, mxc))
    sec["LOOT"] = lb.b

    it = Bin()
    it.u32(len(items))
    for i in items:
        it.u32(i["entry"]); it.s(i["name"][:200]); it.s(i["desc"][:200]); it.u32(i["display"]); it.u32(i["quality"])
        it.u32(i["inv"]); it.u32(i["ilvl"]); it.u32(i["req"]); it.u32(i["stack"]); it.u32(i["buy"]); it.u32(i["sell"])
        it.u32(i["armor"]); it.u32(i["delay"]); it.f32(i["dmin"]); it.f32(i["dmax"]); it.u32(i["cls"]); it.u32(i["sub"])
    sec["ITEM"] = it.b

    vb = Bin()   # WoWVita "VEND": u32 n, n x {u32 creature, u16 count, count x {u32 item, u16 buy count}}
    by_item = {i["entry"]: i for i in items}
    vlist = [(e, [x for x in goods if x in by_item]) for e, goods in sorted(vendors.items())]
    vlist = [(e, gl) for e, gl in vlist if gl]
    vb.u32(len(vlist))
    for e, gl in vlist:
        vb.u32(e); vb.u16(len(gl))
        for x in gl:
            vb.u32(x); vb.u16(min(65535, by_item[x]["buycount"]))
    sec["VEND"] = vb.b

    qb = Bin()   # WoWVita "QST2" (net/offline/offline_data.cpp readQuests2)
    qb.u32(len(quests))
    for q in quests:
        qb.u32(q["id"]); qb.u16(max(0, q["level"]) & 0xFFFF); qb.u16(q["minlevel"]); qb.u32(q["races"]); qb.u32(q["classes"])
        qb.u32(q["zone"] & 0xFFFFFFFF); qb.u32(q["type"]); qb.u32(q["flags"]); qb.u32(q["special"])
        qb.u32(q["prev"] & 0xFFFFFFFF); qb.u32(q["next"]); qb.u32(q["src"]); qb.u8(min(255, q["srccount"]))
        qb.u32(q["money"] & 0xFFFFFFFF); qb.u32(q["moneymax"])
        for t in q["texts"]:
            long_s(qb, t)
        for t in q["objtext"]:
            qb.s(t[:200])
        for c, n in q["kills"]:
            qb.u32(c); qb.u16(n)
        for i, n in q["items"]:
            qb.u32(i); qb.u16(n)
        for i, n in q["cho"]:
            qb.u32(i); qb.u16(n)
        for i, n in q["rew"]:
            qb.u32(i); qb.u16(n)
        qb.u16(len(q["starters"]))
        for c in q["starters"]:
            qb.u32(c)
        qb.u16(len(q["enders"]))
        for c in q["enders"]:
            qb.u32(c)
        qb.u16(len(q["go_starters"]))
        for c in q["go_starters"]:
            qb.u32(c)
        qb.u16(len(q["go_enders"]))
        for c in q["go_enders"]:
            qb.u32(c)
    sec["QST2"] = qb.b

    lq = Bin()   # "QLOT": quest drops per loot table, rolled only while a quest wants the item
    lq.u32(len(qloot))
    for lid in sorted(qloot):
        lq.u32(lid); lq.u16(len(qloot[lid]))
        for i, c in qloot[lid]:
            lq.u32(i); lq.u16(max(1, min(10000, int(round(c * 100)))))
    sec["QLOT"] = lq.b

    ob = Bin()   # "GOBJ": game object templates (the SMSG_GAMEOBJECT_QUERY_RESPONSE content)
    ob.u32(len(go_used))
    for e in go_used:
        t = go_t[e]
        ob.u32(e); ob.u32(int(num(g(t, "type")))); ob.u32(int(num(g(t, "displayId")))); ob.s(str(g(t, "name", default="")))
        ob.u32(int(num(g(t, "faction")))); ob.u32(int(num(g(t, "flags")))); ob.f32(num(g(t, "size", default=1.0)) or 1.0)
        for k in range(24):
            ob.u32(int(num(g(t, f"data{k}"))) & 0xFFFFFFFF)
    sec["GOBJ"] = ob.b
    sb = Bin()   # "GOSP": their spawns
    sb.u32(len(go_spawns))
    for (e, x, y, z, o, rot, respawn) in go_spawns:
        sb.u32(e); sb.f32(x); sb.f32(y); sb.f32(z); sb.f32(o)
        for v in rot:
            sb.f32(v)
        sb.u32(respawn)
    sec["GOSP"] = sb.b
    lb2 = Bin()   # "GLOT": chest loot (u8 1 = only while a quest wants the item)
    lb2.u32(len(go_loot))
    for lid in sorted(go_loot):
        lo, hi, ents = go_loot[lid]
        lb2.u32(lid); lb2.u32(lo); lb2.u32(max(lo, hi)); lb2.u16(len(ents))
        for (i, c, mn, mx, qo) in ents:
            lb2.u32(i); lb2.u16(max(1, min(10000, int(round(c * 100))))); lb2.u16(min(65535, mn)); lb2.u16(min(65535, mx)); lb2.u8(qo)
    sec["GLOT"] = lb2.b

    gb = Bin()   # "GOSS": an NPC's greeting
    gb.u32(len(greetings))
    for e in sorted(greetings):
        gb.u32(e); long_s(gb, greetings[e])
    sec["GOSS"] = gb.b

    # "GRAV": the graveyards of this map (world_safe_locs) and the zones they serve (game_graveyard_zone):
    # a ghost goes to the nearest graveyard linked to the zone it died in that its team may use (0 any, 469 Alliance, 67 Horde)
    locs = {}
    for r in tables.get("world_safe_locs", []):
        if int(num(g(r, "map"))) == map_id:
            locs[int(num(g(r, "id")))] = (num(g(r, "x")), num(g(r, "y")), num(g(r, "z")), num(g(r, "o")))
    links = []
    for r in tables.get("game_graveyard_zone", []):
        gid = int(num(g(r, "id")))
        if gid in locs and int(num(g(r, "link_kind", default=0))) == 0:
            links.append((gid, int(num(g(r, "ghost_loc", "ghost_zone"))), int(num(g(r, "faction")))))
    grb = Bin()
    grb.u32(len(locs))
    for gid in sorted(locs):
        grb.u32(gid)
        for v in locs[gid]:
            grb.f32(v)
    grb.u32(len(links))
    for gid, zone, fac in sorted(links):
        grb.u32(gid); grb.u32(zone); grb.u16(fac)
    sec["GRAV"] = grb.b
    report["counts_graveyards"] = dict(graveyards=len(locs), zone_links=len(links))
    # "TAVN": the inn area triggers (areatrigger_tavern; their shapes are AreaTrigger.dbc, the client's areatriggers.txt)
    tav = sorted({int(num(g(r, "id"))) for r in tables.get("areatrigger_tavern", [])})
    tb = Bin()
    tb.u32(len(tav))
    for t in tav:
        tb.u32(t)
    sec["TAVN"] = tb.b
    sec["SPEL"] = sample["SPEL"]
    behaviour_sections(tables, templates, spawn_rows, spawn_moves, used, sec, report, spell_info)
    group_sections(tables, templates, spawn_rows, spawn_moves, map_id, sec, report)

    xb = Bin()   # "ITX1"
    xb.u32(len(itx))
    for (entry, slots, maxdur, kind, sid, h0, h1, m0, m1, dur, cd, cat, fl) in itx:
        xb.u32(entry); xb.u8(min(slots, 36)); xb.u16(min(maxdur, 65535)); xb.u8(kind); xb.u32(sid)
        xb.u32(h0); xb.u32(h1); xb.u32(m0); xb.u32(m1); xb.u32(dur); xb.u32(cd); xb.u16(cat); xb.u8(fl)
    sec["ITX1"] = xb.b
    tb2 = Bin()   # "QTRG": area trigger -> the exploration quest it completes
    pairs = sorted((t, q["id"]) for q in quests for t in quest_triggers.get(q["id"], []))
    tb2.u32(len(pairs))
    for t, qid in pairs:
        tb2.u32(t); tb2.u32(qid)
    sec["QTRG"] = tb2.b
    eb = Bin()   # "EXPL": exploration_basexp - the XP of discovering an area, by its level
    ex = sorted((int(num(g(r, "level"))), int(num(g(r, "basexp")))) for r in tables.get("exploration_basexp", []))
    eb.u32(len(ex))
    for lv, xp in ex:
        eb.u8(min(lv, 255)); eb.u32(xp)
    sec["EXPL"] = eb.b
    report["counts_items_ext"] = dict(usable=sum(1 for x in itx if x[3]), bags=sum(1 for x in itx if x[1]),
                                      with_durability=sum(1 for x in itx if x[2]))

    hb = Bin()
    hb.u32(len(host))
    for a, b in host:
        hb.u32(a); hb.u32(b)
    sec["HOST"] = hb.b

    info = Bin()   # the sample's start (Northshire Abbey) - the whole map is around it now
    info.u32(map_id); info.u32(9 if map_id == 0 else 0)
    for v in (-8949.95, -132.49, 83.53, 3.07):
        info.f32(v)
    info.s("Eastern Kingdoms (offline)" if map_id == 0 else f"map {map_id} (offline)")
    sec["INFO"] = info.b
    sec["BOTS"] = sample["BOTS"]
    qb = Bin()
    qb.u32(0)           # (no quests: the offline core has no quest dialogs yet)
    sec["QUES"] = qb.b

    report["counts"] = dict(creatures=len(crea), spawns=len(spawn_rows), invisible_skipped=skipped_invisible,
                            items=len(items), loot=len(loot_tables), hostile_factions=hostile_n,
                            neutral_factions=neutral_n, vendors=len(vlist),
                            vendor_items=sum(len(gl) for _, gl in vlist), quests=len(quests),
                            quest_drop_tables=len(qloot), greetings=len(greetings), objects=len(go_used),
                            object_spawns=len(go_spawns), chest_loot=len(go_loot),
                            **{"quests_left_out_" + k.replace(" ", "_").replace("/", ""): v for k, v in skipped_q.items()})
    sec.update(sec_extra)
    return sec


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sql", nargs="+", required=True, help="world dump(s): .sql or .sql.gz")
    ap.add_argument("--factions", required=True, help="factions.txt written by the converter (w3dconv --texts-only)")
    ap.add_argument("--spells", help="spells.txt written by the converter (cast times, ranges, durations of creature spells)")
    ap.add_argument("--glue", help="glue.txt written by the converter (the start outfits of new characters)")
    ap.add_argument("--map", type=int, default=0, help="0 Eastern Kingdoms (default), 1 Kalimdor")
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-compress", action="store_true")
    args = ap.parse_args()

    report = {"errors": [], "warn": []}
    print("reading the dump ...", flush=True)
    tables = load_dump(args.sql)
    print("  " + ", ".join(f"{k}: {len(v)}" for k, v in sorted(tables.items())), flush=True)
    for required in ("creature_template", "creature", "item_template"):
        if not tables.get(required):
            report["errors"].append(f"table {required} not found in the dump")
    if report["errors"]:
        print("\n".join(report["errors"]))
        return 1
    sec = build(tables, load_factions(args.factions), args.map, report, spell_info=load_spell_info(args.spells), glue_path=args.glue)
    write_pack(args.out, sec, compress=not args.no_compress)
    _, back = read_pack(args.out)
    mine = dict(report["counts"])
    validate_sections(back, report)
    report["counts"] = dict(mine, **{k: v for k, v in report.get("counts", {}).items() if k not in mine})
    errs = write_report(args.out + ".report.txt", report)
    print(f"wrote {args.out} ({os.path.getsize(args.out)} bytes): " +
          ", ".join(f"{k}={v}" for k, v in report["counts"].items()) +
          f"; {len(errs)} errors, {len(report['warn'])} warnings (see {args.out}.report.txt)")
    for k in ("counts_movement", "counts_eventai", "counts_groups", "counts_outfits"):
        if k in report:
            print(f"  {k[7:]}: {report[k]}")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
