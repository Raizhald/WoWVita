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

_eod.TAGS = tuple(_eod.TAGS) + ("VEND", "QST2", "QLOT", "GOSS", "GOBJ", "GOSP", "GLOT", "GRAV", "TAVN", "ITX1", "EXPL", "QTRG")   # WoWVita's vendor section (older readers skip unknown tags)

WANTED = {"creature_template", "creature", "creature_loot_template", "reference_loot_template",
          "item_template", "creature_model_info", "creature_spawn_entry", "npc_vendor", "npc_vendor_template",
          "quest_template", "creature_questrelation", "creature_involvedrelation", "gossip_menu", "npc_text",
          "gameobject", "gameobject_template", "gameobject_loot_template", "gameobject_questrelation",
          "gameobject_involvedrelation", "gameobject_spawn_entry", "world_safe_locs", "game_graveyard_zone", "areatrigger_tavern",
          "spell_template", "exploration_basexp", "areatrigger_involvedrelation"}

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


def build(tables, factions, map_id, report, player_faction=1):
    sample = build_sample()
    templates = newest_patch(tables.get("creature_template", []), ("Entry", "entry"))
    items_db = newest_patch(tables.get("item_template", []), ("entry",))
    models = {int(num(g(r, "modelid", "display_id"))): r for r in tables.get("creature_model_info", [])}

    # ---- spawns of the map (vMaNGOS: patch_min/patch_max; only those of the last patch)
    spawn_rows = []
    skipped_invisible = 0
    spawn_entry = {}   # CMaNGOS: id 0 = one of several entries (creature_spawn_entry); the spawn guid picks one
    for r in tables.get("creature_spawn_entry", []):
        spawn_entry.setdefault(int(num(g(r, "guid"))), []).append(int(num(g(r, "entry"))))
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
    sec["SPEL"] = sample["SPEL"]
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
    return sec


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sql", nargs="+", required=True, help="world dump(s): .sql or .sql.gz")
    ap.add_argument("--factions", required=True, help="factions.txt written by the converter (w3dconv --texts-only)")
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
    sec = build(tables, load_factions(args.factions), args.map, report)
    write_pack(args.out, sec, compress=not args.no_compress)
    _, back = read_pack(args.out)
    mine = dict(report["counts"])
    validate_sections(back, report)
    report["counts"] = dict(mine, **{k: v for k, v in report.get("counts", {}).items() if k not in mine})
    errs = write_report(args.out + ".report.txt", report)
    print(f"wrote {args.out} ({os.path.getsize(args.out)} bytes): " +
          ", ".join(f"{k}={v}" for k, v in report["counts"].items()) +
          f"; {len(errs)} errors, {len(report['warn'])} warnings (see {args.out}.report.txt)")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
