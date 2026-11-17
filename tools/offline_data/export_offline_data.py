#!/usr/bin/env python3
"""export_offline_data.py - build the .w3do offline world pack for WoW3DS.

This is the PC-side half of Phase 2. It turns the server-authoritative data that a normal
1.12.1 client never receives (creature templates, spawn points, loot, item templates, a small
spell subset and quest definitions) into one versioned, indexable binary file that the console
loads in a few milliseconds.

Two inputs are supported:

  --sample OUT
      Write the built-in Northshire sample. No game data and no database are needed; it is the
      dataset used by the automated tests and by the fallback path of the 3DS build.

  --sql DIR --out OUT [--map 0] [--tiles x0 y0 x1 y1]
      Read vMaNGOS / CMaNGOS-style SQL dumps (creature_template, creature, creature_loot_template,
      item_template, creature_questrelation, quest_template). Column names are matched by name, so
      minor schema differences do not break the import. Anything unresolved is listed in the
      validation report instead of being silently dropped.

Every run writes a human-readable report (--report FILE, or OUT.report.txt) with:
  * counts per section,
  * referential-integrity errors (a spawn without a template, loot without an item, ...),
  * warnings for columns the importer did not find (with the values it substituted).

Format (little-endian, docs/OFFLINE_DATA_FORMAT.md):
  "W3DO" u16 version u16 flags u32 sectionCount
  sections: u32 tag, u32 offset, u32 size, u32 codec (0 raw, 1 zlib)
"""
import argparse
import os
import re
import struct
import sys
import zlib

MAGIC = b"W3DO"
VERSION = 1
TAGS = ("INFO", "CREA", "SPWN", "LOOT", "ITEM", "SPEL", "QUES", "BOTS", "HOST")


# --------------------------------------------------------------------------- binary writer
class Bin:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v):
        self.b += struct.pack("<B", v & 0xFF)

    def u16(self, v):
        self.b += struct.pack("<H", v & 0xFFFF)

    def u32(self, v):
        self.b += struct.pack("<I", v & 0xFFFFFFFF)

    def i32(self, v):
        self.b += struct.pack("<i", int(v))

    def f32(self, v):
        self.b += struct.pack("<f", float(v))

    def s(self, text):
        raw = (text or "").encode("utf-8")[:500]
        self.u16(len(raw))
        self.b += raw


def write_pack(out_path, sections, compress=False):
    tags = [t for t in TAGS if t in sections]
    header = bytearray()
    header += MAGIC
    header += struct.pack("<HH", VERSION, 0)
    header += struct.pack("<I", len(tags))
    table_size = 16 * len(tags)
    offset = len(header) + table_size
    table = bytearray()
    bodies = bytearray()
    for tag in tags:
        raw = bytes(sections[tag])
        stored = zlib.compress(raw, 9) if compress else raw
        table += tag.encode("ascii")
        table += struct.pack("<III", offset, len(stored), 1 if compress else 0)
        bodies += stored
        offset += len(stored)
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(table)
        f.write(bodies)


def read_pack(path):
    data = open(path, "rb").read()
    if data[:4] != MAGIC:
        raise ValueError("not a W3DO pack")
    version, flags, count = struct.unpack_from("<HHI", data, 4)
    sections = {}
    for i in range(count):
        tag, off, size, codec = struct.unpack_from("<4sIII", data, 12 + 16 * i)
        body = data[off:off + size]
        if codec == 1:
            body = zlib.decompress(body)
        sections[tag.decode("ascii")] = body
    return version, sections


# --------------------------------------------------------------------------- sample data
def build_sample():
    """The same readable Northshire dataset the C++ fallback builds (net/offline/offline_data.cpp)."""
    sec = {}
    name = "Northshire Abbey (offline sample)"

    info = Bin()
    info.u32(0)          # map: Eastern Kingdoms
    info.u32(9)          # zone: Northshire Valley
    for v in (-8949.95, -132.49, 83.53, 3.07):
        info.f32(v)
    info.s(name)
    sec["INFO"] = info.b

    creatures = [
        # entry, name, subname, display, minlvl, maxlvl, faction, hp, armor, xp, dmgmin, dmgmax,
        # attackms, powertype, power, rank, ctype, npcflags, scale, speed, aggro, reach, bound, loot, abilities
        (299, "Young Wolf", "", 903, 1, 1, 32, 40, 0, 45, 2, 4, 2000, 0, 0, 0, 1, 0, 1.0, 7.0, 12.0, 1.5, 0.5, 1, []),
        (6, "Kobold Vermin", "", 41, 1, 1, 32, 55, 0, 50, 2, 5, 1900, 0, 200, 0, 7, 0, 1.0, 7.0, 12.0, 1.5, 0.5, 2, [133]),
        (73, "Defias Thug", "", 377, 2, 2, 14, 60, 0, 60, 3, 6, 2100, 0, 0, 0, 7, 0, 1.0, 7.0, 12.0, 1.5, 0.5, 3, []),
        (197, "Marshal McBride", "Northshire Guard", 1297, 30, 30, 12, 500, 0, 0, 8, 12, 2000, 0, 0, 0, 7, 3, 1.0, 7.0, 0.0, 1.5, 0.5, 0, []),
    ]
    c = Bin()
    c.u32(len(creatures))
    for (entry, nm, sub, disp, mn, mx, fac, hp, armor, xp, dmin, dmax, atk, pt, pw, rank, ct, npc, scale, spd, aggro, reach, bound, loot, abil) in creatures:
        c.u32(entry); c.s(nm); c.s(sub); c.u32(disp); c.u16(mn); c.u16(mx); c.u32(fac)
        c.u32(hp); c.u32(armor); c.u32(xp); c.u16(dmin); c.u16(dmax); c.u32(atk)
        c.u8(pt); c.u32(pw); c.u8(rank); c.u8(ct); c.u32(npc)
        c.f32(scale); c.f32(spd); c.f32(aggro); c.f32(reach); c.f32(bound)
        c.u32(loot); c.u8(len(abil))
        for a in abil:
            c.u32(a)
    sec["CREA"] = c.b

    spawns = [(299, 9, 4, 0, 1.0), (299, 15, -3, 0, 2.0), (299, 21, 6, 0, 4.0), (299, 6, -9, 0, 0.5),
              (6, -10, 8, 0, 3.0), (6, -18, 2, 0, 5.0), (6, 2, -14, 0, 2.5),
              (73, 26, -8, 0, 3.5), (73, 31, -2, 0, 4.5), (197, 3, -2, 0, 1.5)]
    sp = Bin()
    sp.u32(len(spawns))
    for entry, dx, dy, dz, o in spawns:
        sp.u32(entry)
        sp.f32(-8949.95 + dx); sp.f32(-132.49 + dy); sp.f32(83.53 + dz); sp.f32(o)
        sp.u32(30000); sp.u8(0)
    sec["SPWN"] = sp.b

    loot = [(1, 5, 23, [(750, 6000, 1, 2), (2589, 3000, 1, 2)]),
            (2, 4, 18, [(2589, 4000, 1, 1)]),
            (3, 10, 35, [(2589, 5000, 1, 2), (25, 1500, 1, 1)])]
    lt = Bin()
    lt.u32(len(loot))
    for lid, lo, hi, items in loot:
        lt.u32(lid); lt.u32(lo); lt.u32(hi); lt.u16(len(items))
        for item, chance, mn, mx in items:
            lt.u32(item); lt.u16(chance); lt.u16(mn); lt.u16(mx)
    sec["LOOT"] = lt.b

    items = [
        (25, "Worn Shortsword", "", 1542, 1, 21, 1, 0, 1, 4, 1, 0, 1900, 1.0, 3.0, 2, 7),
        (38, "Recruit's Shirt", "", 9900, 1, 4, 1, 0, 1, 4, 1, 0, 0, 0.0, 0.0, 2, 0),
        (2589, "Linen Cloth", "", 6803, 1, 0, 1, 0, 20, 52, 13, 0, 0, 0.0, 0.0, 2, 0),
        (750, "Tough Wolf Meat", "", 6362, 1, 0, 1, 0, 10, 16, 4, 0, 0, 0.0, 0.0, 2, 0),
    ]
    it = Bin()
    it.u32(len(items))
    for (entry, nm, desc, disp, q, inv, ilvl, req, stack, buy, sell, armor, delay, dmin, dmax, cls, sub) in items:
        it.u32(entry); it.s(nm); it.s(desc); it.u32(disp); it.u32(q); it.u32(inv); it.u32(ilvl); it.u32(req)
        it.u32(stack); it.u32(buy); it.u32(sell); it.u32(armor); it.u32(delay); it.f32(dmin); it.f32(dmax)
        it.u32(cls); it.u32(sub)
    sec["ITEM"] = it.b

    # id, name, kind, school, castMs, cd, category, catCd, minR, maxR, powerType, cost,
    # amountMin, amountMax, duration, tick, threat, radius, friendly, autoatk
    spells = [
        (6603, "Attack", 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 1, 2, 0, 0, 0, 0.0, 0, 1),
        (78, "Heroic Strike", 0, 0, 0, 0, 0, 0, 0, 5, 1, 15, 8, 12, 0, 0, 20, 0.0, 0, 1),
        (6673, "Battle Shout", 4, 0, 0, 0, 0, 0, 0, 0, 1, 10, 0, 0, 120000, 0, 0, 0.0, 0, 0),
        (355, "Taunt", 5, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 200, 0.0, 0, 0),
        (2050, "Lesser Heal", 2, 1, 1500, 0, 0, 0, 0, 40, 0, 30, 40, 50, 0, 0, 0, 0.0, 1, 0),
        (2061, "Flash Heal", 2, 1, 1500, 0, 0, 0, 0, 40, 0, 45, 70, 85, 0, 0, 0, 0.0, 1, 0),
        (585, "Smite", 1, 1, 2000, 0, 0, 0, 0, 30, 0, 20, 12, 16, 0, 0, 0, 0.0, 0, 0),
        (1243, "Power Word: Fortitude", 4, 1, 0, 0, 0, 0, 0, 30, 0, 20, 0, 0, 1800000, 0, 0, 0.0, 1, 0),
        (133, "Fireball", 1, 2, 2500, 0, 0, 0, 0, 35, 0, 30, 14, 22, 0, 0, 0, 0.0, 0, 0),
        (116, "Frostbolt", 1, 4, 1800, 0, 0, 0, 0, 30, 0, 25, 12, 18, 0, 0, 0, 0.0, 0, 0),
        (2136, "Fire Blast", 1, 2, 0, 8000, 0, 0, 0, 20, 0, 25, 12, 16, 0, 0, 0, 0.0, 0, 0),
        (589, "Shadow Word: Pain", 3, 5, 0, 0, 0, 0, 0, 30, 0, 25, 6, 6, 18000, 2000, 0, 0.0, 0, 0),
        (1752, "Sinister Strike", 0, 0, 0, 0, 0, 0, 0, 5, 3, 45, 10, 14, 0, 0, 0, 0.0, 0, 1),
        (2098, "Eviscerate", 0, 0, 0, 0, 0, 0, 0, 5, 3, 35, 15, 20, 0, 0, 0, 0.0, 0, 1),
        (1766, "Kick", 6, 0, 0, 10000, 0, 0, 0, 5, 3, 25, 0, 0, 0, 0, 0, 0.0, 0, 0),
    ]
    sd = Bin()
    sd.u32(len(spells))
    for (sid, nm, kind, school, cast, cd, cat, catcd, mnr, mxr, pt, cost, lo, hi, dur, tick, threat, radius, friendly, autoatk) in spells:
        sd.u32(sid); sd.s(nm); sd.u8(kind); sd.u8(school); sd.u32(cast); sd.u32(cd); sd.u32(cat); sd.u32(catcd)
        sd.f32(mnr); sd.f32(mxr); sd.u8(pt); sd.u32(cost); sd.i32(lo); sd.i32(hi); sd.u32(dur); sd.u32(tick)
        sd.i32(threat); sd.f32(radius); sd.u8(friendly); sd.u8(autoatk)
    sec["SPEL"] = sd.b

    quests = [(7, "Kobold Camp Cleanup", "Kobold vermin have been seen near the abbey. Thin their numbers.",
               "The abbey is safer already.", 2, 197, 0, 6, 5, "Kobold Vermin slain", 250, 100,
               [2589, 0, 0], [3, 0, 0])]
    q = Bin()
    q.u32(len(quests))
    for (qid, title, details, comp, lvl, giver, kind, target, count, text, xp, money, items3, counts3) in quests:
        q.u32(qid); q.s(title); q.s(details); q.s(comp); q.u16(lvl); q.u32(giver)
        q.u8(kind); q.u32(target); q.u16(count); q.s(text); q.u32(xp); q.u32(money)
        for i in items3:
            q.u32(i)
        for i in counts3:
            q.u16(i)
    sec["QUES"] = q.b

    bots = [
        ("Brom", 1, 1, 0, 1, 1, 0, 0, 0, 0, 10, 320, 100, 1, 49, [6603, 78, 355, 6673], 3.5),
        ("Elara", 1, 5, 1, 1, 1, 0, 0, 0, 1, 10, 240, 260, 0, 50, [585, 2050, 2061, 1243], 6.0),
        ("Mira", 7, 8, 1, 1, 1, 0, 0, 0, 3, 10, 200, 300, 0, 60, [133, 116, 2136], 6.0),
        ("Rik", 3, 4, 0, 1, 1, 0, 0, 0, 2, 10, 230, 100, 3, 51, [6603, 1752, 2098, 1766], 6.0),
    ]
    b = Bin()
    b.u8(len(bots))
    for (nm, race, cls, gender, skin, face, hs, hc, fh, role, lvl, hp, power, pt, disp, spl, follow) in bots:
        b.s(nm); b.u8(race); b.u8(cls); b.u8(gender); b.u8(skin); b.u8(face); b.u8(hs); b.u8(hc); b.u8(fh)
        b.u8(role); b.u16(lvl); b.u32(hp); b.u32(power); b.u8(pt); b.u32(disp); b.u8(len(spl))
        for s in spl:
            b.u32(s)
        eq = [0] * 19
        eq[3] = 38
        eq[15] = 25
        for e in eq:
            b.u32(e)
        b.f32(follow)
    sec["BOTS"] = b.b

    host = Bin()
    pairs = [(1, 32), (32, 1), (1, 14), (14, 1)]
    host.u32(len(pairs))
    for a, bb in pairs:
        host.u32(a); host.u32(bb)
    sec["HOST"] = host.b
    return sec


# --------------------------------------------------------------------------- SQL import
SQL_TUPLE = re.compile(r"\(([^()]*)\)")


def split_sql_values(text):
    """Split a VALUES (...) , (...) body into lists of Python-ish values."""
    rows = []
    i = 0
    n = len(text)
    while i < n:
        if text[i] != "(":
            i += 1
            continue
        i += 1
        depth = 1
        cur = []
        in_str = False
        while i < n and depth:
            ch = text[i]
            if in_str:
                if ch == "\\" and i + 1 < n:
                    cur.append(text[i + 1])
                    i += 2
                    continue
                if ch == "'":
                    if i + 1 < n and text[i + 1] == "'":
                        cur.append("'")
                        i += 2
                        continue
                    in_str = False
                    i += 1
                    continue
                cur.append(ch)
                i += 1
                continue
            if ch == "'":
                in_str = True
                cur.append("\x00")  # marker: this value was a string
                i += 1
                continue
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    i += 1
                    break
            cur.append(ch)
            i += 1
        row = []
        val = ""
        was_str = False
        for ch in cur:
            if ch == ",":
                row.append((was_str, val.strip()))
                val, was_str = "", False
            else:
                if ch == "\x00":
                    was_str = True
                else:
                    val += ch
        row.append((was_str, val.strip()))
        out = []
        for is_str, v in row:
            if is_str:
                out.append(v)
            elif v.upper() == "NULL":
                out.append(None)
            else:
                try:
                    out.append(int(v))
                except ValueError:
                    try:
                        out.append(float(v))
                    except ValueError:
                        out.append(v)
        rows.append(out)
    return rows


def load_sql_tables(directory):
    tables = {}
    for fn in sorted(os.listdir(directory)):
        if not fn.lower().endswith(".sql"):
            continue
        path = os.path.join(directory, fn)
        text = open(path, "r", encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"INSERT\s+INTO\s+`?(\w+)`?\s*\(([^)]*)\)\s*VALUES\s*(.*?);", text, re.S | re.I):
            table = m.group(1).lower()
            cols = [c.strip().strip("`") for c in m.group(2).split(",")]
            rows = split_sql_values(m.group(3))
            dst = tables.setdefault(table, [])
            for r in rows:
                if len(r) != len(cols):
                    continue
                dst.append(dict(zip(cols, r)))
    return tables


def get(row, *names, default=None):
    for n in names:
        for k, v in row.items():
            if k.lower() == n.lower() and v is not None:
                return v
    return default


def build_from_sql(tables, map_id, bounds, report):
    sec = {}
    creatures = tables.get("creature_template", [])
    spawns = tables.get("creature", [])
    loot = tables.get("creature_loot_template", [])
    items = tables.get("item_template", [])
    questrel = tables.get("creature_questrelation", [])
    quests = tables.get("quest_template", [])

    # ---- items used by loot and the four bots
    item_rows = []
    for row in items:
        entry = int(get(row, "entry", "Entry", default=0) or 0)
        if not entry:
            continue
        inv = int(get(row, "InventoryType", "inventory_type", default=0) or 0)
        dmg_min = float(get(row, "dmg_min1", "dmg_min", default=0) or 0)
        dmg_max = float(get(row, "dmg_max1", "dmg_max", default=0) or 0)
        item_rows.append(dict(
            entry=entry, name=str(get(row, "name", default=f"Item {entry}")),
            desc=str(get(row, "description", default="")),
            display=int(get(row, "displayid", "display_id", default=0) or 0),
            quality=int(get(row, "Quality", "quality", default=1) or 1),
            inv=inv, ilvl=int(get(row, "ItemLevel", "item_level", default=1) or 1),
            req=int(get(row, "RequiredLevel", "required_level", default=0) or 0),
            stack=int(get(row, "stackable", "max_stack", default=1) or 1),
            buy=int(get(row, "BuyPrice", "buy_price", default=0) or 0),
            sell=int(get(row, "SellPrice", "sell_price", default=0) or 0),
            armor=int(get(row, "armor", default=0) or 0),
            delay=int(get(row, "delay", "Delay", default=0) or 0),
            dmg_min=dmg_min, dmg_max=dmg_max,
            cls=int(get(row, "class", "class_", default=2) or 2),
            sub=int(get(row, "subclass", "subclass_", default=0) or 0)))
    known_items = {i["entry"] for i in item_rows}
    it = Bin()
    it.u32(len(item_rows))
    for i in item_rows:
        it.u32(i["entry"]); it.s(i["name"]); it.s(i["desc"]); it.u32(i["display"]); it.u32(i["quality"])
        it.u32(i["inv"]); it.u32(i["ilvl"]); it.u32(i["req"]); it.u32(max(1, i["stack"]))
        it.u32(i["buy"]); it.u32(i["sell"]); it.u32(i["armor"]); it.u32(i["delay"])
        it.f32(i["dmg_min"]); it.f32(i["dmg_max"]); it.u32(i["cls"]); it.u32(i["sub"])
    sec["ITEM"] = it.b

    # ---- creature templates
    templates = []
    for row in creatures:
        entry = int(get(row, "entry", default=0) or 0)
        if not entry:
            continue
        mn = int(get(row, "minlevel", "min_level", default=1) or 1)
        mx = int(get(row, "maxlevel", "max_level", default=mn) or mn)
        hp = int(get(row, "minhealth", "health_min", "minhealth", default=0) or 0)
        if not hp:
            hp = int(get(row, "maxhealth", "health_max", default=100) or 100)
        mana = int(get(row, "minmana", "mana_min", default=0) or 0)
        dmg_min = int(get(row, "mindmg", "dmg_min", default=2) or 2)
        dmg_max = int(get(row, "maxdmg", "dmg_max", default=4) or 4)
        templates.append(dict(
            entry=entry, name=str(get(row, "name", default=f"Creature {entry}")),
            subname=str(get(row, "subname", default="") or ""),
            display=int(get(row, "display_id1", "displayid", "modelid1", default=0) or 0),
            minlvl=mn, maxlvl=mx,
            faction=int(get(row, "faction", "faction_a", default=0) or 0),
            hp=max(1, hp), armor=int(get(row, "armor", default=0) or 0),
            xp=0,
            dmg_min=max(1, dmg_min), dmg_max=max(1, dmg_max),
            attack=int(get(row, "baseattacktime", "attack_time", default=2000) or 2000),
            pt=0, power=mana,
            rank=int(get(row, "rank", default=0) or 0),
            ctype=int(get(row, "type", "creature_type", default=7) or 7),
            npc=int(get(row, "npcflag", "npc_flags", default=0) or 0),
            scale=float(get(row, "scale", default=1.0) or 1.0),
            speed=float(get(row, "speed_run", default=1.14286) or 1.14286) * 7.0,
            aggro=float(get(row, "aggro_range", default=12.0) or 12.0),
            reach=1.5, bound=0.5,
            loot=int(get(row, "lootid", "loot_id", default=0) or 0),
            abilities=[]))
    known_creatures = {t["entry"] for t in templates}
    cb = Bin()
    cb.u32(len(templates))
    for t in templates:
        cb.u32(t["entry"]); cb.s(t["name"]); cb.s(t["subname"]); cb.u32(t["display"])
        cb.u16(t["minlvl"]); cb.u16(t["maxlvl"]); cb.u32(t["faction"]); cb.u32(t["hp"])
        cb.u32(t["armor"]); cb.u32(t["xp"]); cb.u16(t["dmg_min"]); cb.u16(t["dmg_max"])
        cb.u32(t["attack"]); cb.u8(t["pt"]); cb.u32(t["power"]); cb.u8(t["rank"]); cb.u8(t["ctype"])
        cb.u32(t["npc"]); cb.f32(t["scale"]); cb.f32(t["speed"]); cb.f32(t["aggro"])
        cb.f32(t["reach"]); cb.f32(t["bound"]); cb.u32(t["loot"]); cb.u8(0)
    sec["CREA"] = cb.b

    # ---- spawns (optionally clipped to a tile rectangle: 64 tiles per continent)
    sp_rows = []
    for row in spawns:
        entry = int(get(row, "id", "entry", default=0) or 0)
        if entry not in known_creatures:
            report["warn"].append(f"spawn references unknown creature template {entry}")
            continue
        x = float(get(row, "position_x", "pos_x", default=0) or 0)
        y = float(get(row, "position_y", "pos_y", default=0) or 0)
        z = float(get(row, "position_z", "pos_z", default=0) or 0)
        o = float(get(row, "orientation", default=0) or 0)
        if int(get(row, "map", "Map", default=map_id) or map_id) != map_id:
            continue
        if bounds:
            tx = 32.0 - y / 533.3333
            ty = 32.0 - x / 533.3333
            if not (bounds[0] <= tx <= bounds[2] and bounds[1] <= ty <= bounds[3]):
                continue
        respawn = int(get(row, "spawntimesecs", "spawntime", default=25) or 25) * 1000
        sp_rows.append((entry, x, y, z, o, max(5000, respawn)))
    spb = Bin()
    spb.u32(len(sp_rows))
    for (entry, x, y, z, o, respawn) in sp_rows:
        spb.u32(entry); spb.f32(x); spb.f32(y); spb.f32(z); spb.f32(o); spb.u32(respawn); spb.u8(0)
    sec["SPWN"] = spb.b

    # ---- loot tables
    by_table = {}
    for row in loot:
        entry = int(get(row, "entry", "lootid", default=0) or 0)
        item = int(get(row, "item", "Item", default=0) or 0)
        if not entry or not item:
            continue
        if item not in known_items:
            report["warn"].append(f"loot {entry} references unknown item {item}")
            continue
        chance = float(get(row, "ChanceOrQuestChance", "chance", default=0) or 0)
        mn = int(get(row, "mincountOrRef", "mincount", default=1) or 1)
        mx = int(get(row, "maxcount", default=mn) or mn)
        by_table.setdefault(entry, []).append((item, int(abs(chance) * 100), max(1, mn), max(1, mx)))
    lb = Bin()
    lb.u32(len(by_table))
    for lid, items_ in sorted(by_table.items()):
        lb.u32(lid); lb.u32(0); lb.u32(30); lb.u16(len(items_))
        for (item, chance, mn, mx) in items_:
            lb.u32(item); lb.u16(min(10000, chance)); lb.u16(mn); lb.u16(mx)
    sec["LOOT"] = lb.b

    # ---- quests (kill/collect objectives only)
    qrows = []
    for row in quests:
        qid = int(get(row, "entry", default=0) or 0)
        if not qid:
            continue
        giver = 0
        for rel in questrel:
            if int(get(rel, "quest", default=0) or 0) == qid:
                giver = int(get(rel, "id", "creature_id", default=0) or 0)
                break
        req_creature = int(get(row, "ReqCreatureOrGOId1", "req_creature_or_go_id1", default=0) or 0)
        req_item = int(get(row, "ReqItemId1", "req_item_id1", default=0) or 0)
        if req_creature:
            kind, target = 0, abs(req_creature)
            text = str(get(row, "ObjectiveText1", default="") or "slain")
        elif req_item:
            kind, target = 1, req_item
            text = str(get(row, "ObjectiveText1", default="") or "collected")
        else:
            continue
        count = int(get(row, "ReqCreatureOrGOCount1", "req_creature_or_go_count1",
                        "ReqItemCount1", "req_item_count1", default=1) or 1)
        qrows.append(dict(id=qid, title=str(get(row, "Title", default=f"Quest {qid}")),
                          details=str(get(row, "Details", default="")),
                          comp=str(get(row, "CompletionText", default="")),
                          level=int(get(row, "QuestLevel", "quest_level", default=1) or 1),
                          giver=giver, kind=kind, target=target, count=count, text=text,
                          xp=int(get(row, "RewardXP", "reward_xp", default=0) or 0),
                          money=int(get(row, "RewardMoney", "reward_money", default=0) or 0),
                          items=[int(get(row, f"RewardItem{i}", default=0) or 0) for i in (1, 2, 3)],
                          counts=[int(get(row, f"RewardAmount{i}", default=1) or 1) for i in (1, 2, 3)]))
    qb = Bin()
    qb.u32(len(qrows))
    for q in qrows:
        qb.u32(q["id"]); qb.s(q["title"]); qb.s(q["details"]); qb.s(q["comp"]); qb.u16(q["level"])
        qb.u32(q["giver"]); qb.u8(q["kind"]); qb.u32(q["target"]); qb.u16(q["count"]); qb.s(q["text"])
        qb.u32(q["xp"]); qb.u32(q["money"])
        for i in q["items"]:
            qb.u32(i)
        for i in q["counts"]:
            qb.u16(i)
    sec["QUES"] = qb.b

    # ---- spells: the documented supported subset (same ids as the sample)
    sample = build_sample()
    sec["SPEL"] = sample["SPEL"]
    sec["BOTS"] = sample["BOTS"]

    # ---- info from the spawn point
    spawn_x = spawn_y = spawn_z = 0.0
    spawn_o = 0.0
    if sp_rows:
        spawn_x, spawn_y, spawn_z, spawn_o = sp_rows[0][1], sp_rows[0][2], sp_rows[0][3], sp_rows[0][4]
    info = Bin()
    info.u32(map_id); info.u32(0)
    info.f32(spawn_x); info.f32(spawn_y); info.f32(spawn_z); info.f32(spawn_o)
    info.s(f"map {map_id}, {len(sp_rows)} spawns")
    sec["INFO"] = info.b
    sec["HOST"] = build_sample()["HOST"]
    report["counts"] = dict(items=len(item_rows), creatures=len(templates), spawns=len(sp_rows),
                            loot=len(by_table), quests=len(qrows))
    return sec


# --------------------------------------------------------------------------- validation
def validate_sections(sec, report):
    """Independent referential-integrity pass over a written pack (the --verify mode)."""
    class Cur:
        def __init__(self, b):
            self.b = b
            self.p = 0

        def take(self, fmt, n):
            v = struct.unpack_from(fmt, self.b, self.p)
            self.p += n
            return v

        def u8(self):
            return self.take("<B", 1)[0]

        def u16(self):
            return self.take("<H", 2)[0]

        def u32(self):
            return self.take("<I", 4)[0]

        def i32(self):
            return self.take("<i", 4)[0]

        def f32(self):
            return self.take("<f", 4)[0]

        def s(self):
            n = self.u16()
            raw = self.b[self.p:self.p + n]
            self.p += n
            return raw.decode("utf-8", "replace")

    creatures = {}
    c = Cur(sec["CREA"])
    n = c.u32()
    for _ in range(n):
        entry = c.u32()
        name = c.s()
        c.s()
        c.u32(); c.u16(); c.u16(); c.u32(); c.u32(); c.u32(); c.u32(); c.u16(); c.u16(); c.u32()
        c.u8(); c.u32(); c.u8(); c.u8(); c.u32()
        for _ in range(5):
            c.f32()
        loot_id = c.u32()
        ac = c.u8()
        abilities = [c.u32() for _ in range(ac)]
        creatures[entry] = dict(name=name, abilities=abilities, loot=loot_id)

    spawns = 0
    loot_ids = set()
    sp = Cur(sec["SPWN"])
    n = sp.u32()
    for _ in range(n):
        entry = sp.u32()
        for _ in range(4):
            sp.f32()
        sp.u32(); sp.u8()
        spawns += 1
        if entry not in creatures:
            report["errors"].append(f"SPWN: unknown creature {entry}")

    items = set()
    it = Cur(sec["ITEM"]) if "ITEM" in sec else None
    if it:
        n = it.u32()
        for _ in range(n):
            entry = it.u32()
            it.s(); it.s()
            for _ in range(10):
                it.u32()
            for _ in range(2):
                it.f32()
            it.u32(); it.u32()
            items.add(entry)

    if "LOOT" in sec:
        lb = Cur(sec["LOOT"])
        n = lb.u32()
        for _ in range(n):
            lid = lb.u32()
            lb.u32(); lb.u32()
            c2 = lb.u16()
            for _ in range(c2):
                item = lb.u32()
                lb.u16(); lb.u16(); lb.u16()
                if item not in items:
                    report["errors"].append(f"LOOT {lid}: unknown item {item}")
            loot_ids.add(lid)

    for entry, cr in creatures.items():
        if cr["loot"] and cr["loot"] not in loot_ids:
            report["errors"].append(f"CREA {entry}: loot table {cr['loot']} is missing")

    spells = set()
    sb = Cur(sec["SPEL"])
    n = sb.u32()
    for _ in range(n):
        sid = sb.u32()
        sb.s()
        sb.u8(); sb.u8()
        for _ in range(4):
            sb.u32()
        sb.f32(); sb.f32()
        sb.u8()
        sb.u32()
        sb.i32(); sb.i32()
        sb.u32(); sb.u32(); sb.i32(); sb.f32()
        sb.u8(); sb.u8()
        spells.add(sid)
    for entry, cr in creatures.items():
        for a in cr["abilities"]:
            if a and a not in spells:
                report["errors"].append(f"CREA {entry}: unknown ability {a}")

    bb = Cur(sec["BOTS"])
    nb = bb.u8()
    for _ in range(nb):
        nm = bb.s()
        for _ in range(9):
            bb.u8()
        bb.u16(); bb.u32(); bb.u32(); bb.u8(); bb.u32()
        sc = bb.u8()
        bs = [bb.u32() for _ in range(sc)]
        for _ in range(19):
            bb.u32()
        bb.f32()
        for s in bs:
            if s not in spells:
                report["errors"].append(f"BOTS {nm}: unknown spell {s}")

    report["counts"] = dict(creatures=len(creatures), spawns=spawns, items=len(items), spells=len(spells), bots=nb)


def write_report(path, report):
    lines = ["offline world pack report", "==========================", ""]
    if report.get("counts"):
        lines.append("counts: " + ", ".join(f"{k}={v}" for k, v in report["counts"].items()))
    lines.append("")
    lines.append(f"errors: {len(report['errors'])}")
    lines += ["  " + e for e in report["errors"][:200]]
    lines.append(f"warnings: {len(report['warn'])}")
    lines += ["  " + w for w in report["warn"][:200]]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return report["errors"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sample", metavar="OUT", help="write the built-in sample pack")
    ap.add_argument("--sql", metavar="DIR", help="directory with vMaNGOS/CMaNGOS SQL dumps")
    ap.add_argument("--out", metavar="OUT")
    ap.add_argument("--map", type=int, default=0)
    ap.add_argument("--tiles", type=float, nargs=4, metavar=("X0", "Y0", "X1", "Y1"))
    ap.add_argument("--compress", action="store_true")
    ap.add_argument("--report", metavar="FILE")
    ap.add_argument("--verify", metavar="PACK", help="re-read a pack and validate it")
    args = ap.parse_args()

    if args.verify:
        version, sec = read_pack(args.verify)
        report = {"errors": [], "warn": []}
        missing = [t for t in ("INFO", "CREA", "SPWN", "BOTS") if t not in sec]
        report["errors"] += [f"missing section {t}" for t in missing]
        if not missing:
            validate_sections(sec, report)
        errs = write_report(args.verify + ".report.txt", report)
        print(f"verified {args.verify} (version {version}): {len(errs)} errors")
        return 1 if errs else 0

    out = args.out or (args.sample if args.sample else None)
    if not out:
        ap.error("either --sample OUT or --sql DIR --out OUT is required")
    report = {"errors": [], "warn": []}
    if args.sample:
        sec = build_sample()
        report["counts"] = dict(creatures=4, spawns=10, items=4, spells=15, quests=1)
    else:
        tables = load_sql_tables(args.sql)
        if not tables:
            ap.error(f"no SQL tables found in {args.sql}")
        for required in ("creature_template", "creature"):
            if required not in tables:
                report["warn"].append(f"table '{required}' not found: that section will be empty")
        sec = build_from_sql(tables, args.map, args.tiles, report)

    write_pack(out, sec, compress=args.compress)
    version, back = read_pack(out)
    report["errors"] += [f"missing section {t}" for t in ("INFO", "CREA", "SPWN", "BOTS") if t not in back]
    if not report["errors"]:
        validate_sections(back, report)
    report_path = args.report or (out + ".report.txt")
    errs = write_report(report_path, report)
    print(f"wrote {out} ({os.path.getsize(out)} bytes); report: {report_path}; {len(errs)} errors, "
          f"{len(report['warn'])} warnings")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
