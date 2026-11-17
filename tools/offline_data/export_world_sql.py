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
from export_offline_data import (Bin, build_sample, read_pack, split_sql_values,  # noqa: E402
                                 validate_sections, write_pack, write_report)

WANTED = {"creature_template", "creature", "creature_loot_template", "reference_loot_template",
          "item_template", "creature_model_info", "creature_spawn_entry"}

CREATE_RE = re.compile(r"^CREATE TABLE `(\w+)`")
COL_RE = re.compile(r"^\s+`(\w+)`")
INSERT_RE = re.compile(r"^INSERT INTO `(\w+)`\s*(\(([^)]*)\))?\s*VALUES\s*", re.I)


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
                for r in split_sql_values(body):
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
                if chance < 0:
                    continue                                   # quest item (no quests offline)
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
    for lid in sorted(loot_ids):
        ents = [e for e in entries_of(by_entry.get(lid, [])) if e[0] in items_db]
        ents.sort(key=lambda e: -e[1])
        ents = ents[:64]
        for e in ents:
            item_needed.add(e[0])
        lo, hi = money_by_loot.get(lid, (0, 0))
        loot_tables.append((lid, lo, max(lo, hi), ents))

    # ---- items: what the loot and the companions need (the sample's item ids are in the DB too)
    for e in (25, 38, 2589, 750):
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
            cls=int(num(g(r, "class"))), sub=int(num(g(r, "subclass")))))
    known_items = {i["entry"] for i in items}

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
                            neutral_factions=neutral_n)
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
