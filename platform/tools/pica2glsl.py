#!/usr/bin/env python3
"""pica2glsl.py - translate a picasso vertex shader (.v.pica) into GLSL ES 1.00 and Cg.

Writes <name>_shbin.h with the same symbol names picasso + bin2s produce (<name>_shbin, <name>_shbin_size), so the
game's DVLB_ParseFile / shaderInstanceGetUniformLocation code stays unchanged. The "shbin" is a DVLB_s of the
compatibility layer (platform/include/3ds.h) holding both sources and the uniform register table.

Semantics follow the PICA200 vertex unit:
  * float uniforms (.fvec) take consecutive registers c0.. in declaration order (arrays take one per element);
  * a source swizzle is padded to four components by repeating its last one, and destination component k reads
    padded swizzle[k] (so "mov r4.z, r5.y" broadcasts r5.y);
  * dp3 / dp4 / rsq / rcp write one scalar to every component of the write mask;
  * mova sets the address registers a0.x / a0.y used by relative addressing ("bones[a0.x+1]").
Output position z is remapped from PICA's [-w, 0] (near -> -1, read back as depth = -z/w) to GL's [-w, w] keeping
near = 1 / far = 0, so the game's GREATER depth tests and zero depth clears work unchanged.

usage: pica2glsl.py input.v.pica output_dir
"""
import os
import re
import sys

COMPS = "xyzw"


class Err(Exception):
    pass


def pad_swz(s):
    s = s or "xyzw"
    s = s.replace("r", "x").replace("g", "y").replace("b", "z").replace("a", "w")
    while len(s) < 4:
        s += s[-1]
    return s


class Translator:
    def __init__(self, name, text):
        self.name = name
        self.text = text
        self.uniforms = []        # (name, base, count)
        self.ureg = {}            # name -> (base, count)
        self.nregs = 0
        self.consts = {}          # name -> [4 floats]
        self.aliases = {}         # name -> (target, swizzle or None)
        self.outs = {}            # name -> semantic
        self.inputs = set()       # v indices used
        self.temps = set()
        self.body = []            # list of IR statements (dest, destmask, kind, srcs)
        self.uses_a0 = False

    # ------------------------------------------------------------ parsing
    def parse(self):
        in_proc = False
        for raw in self.text.splitlines():
            line = raw.split(";", 1)[0].strip()
            if not line:
                continue
            if line.startswith(".fvec"):
                for decl in line[5:].split(","):
                    decl = decl.strip()
                    m = re.fullmatch(r"(\w+)(?:\[(\d+)\])?", decl)
                    if not m:
                        raise Err("bad .fvec: " + decl)
                    n = int(m.group(2) or 1)
                    self.uniforms.append((m.group(1), self.nregs, n))
                    self.ureg[m.group(1)] = (self.nregs, n)
                    self.nregs += n
            elif line.startswith(".constf"):
                m = re.fullmatch(r"\.constf\s+(\w+)\s*\(([^)]*)\)", line)
                vals = [float(v) for v in m.group(2).split(",")]
                self.consts[m.group(1)] = vals
            elif line.startswith(".alias"):
                m = re.fullmatch(r"\.alias\s+(\w+)\s+([\w\[\]+]+)(?:\.(\w+))?", line)
                self.aliases[m.group(1)] = (m.group(2), m.group(3))
            elif line.startswith(".out"):
                m = re.fullmatch(r"\.out\s+(\w+)\s+(\w+)(?:\.\w+)?", line)
                self.outs[m.group(1)] = m.group(2)
            elif line.startswith(".proc"):
                in_proc = True
            elif line.startswith(".end"):
                in_proc = False
            elif line.startswith("."):
                raise Err("unsupported directive: " + line)
            elif in_proc:
                self.instr(line)

    def instr(self, line):
        m = re.fullmatch(r"(\w+)\s*(.*)", line)
        op, rest = m.group(1).lower(), m.group(2)
        if op in ("end", "nop"):
            return
        args = [a.strip() for a in re.split(r",(?![^\[]*\])", rest)] if rest else []
        if op not in ("mov", "add", "mul", "max", "min", "dp3", "dp4", "rsq", "rcp", "mova", "flr", "slt", "sge"):
            raise Err("unsupported instruction: " + line)
        dest = args[0]
        self.body.append((op, dest, args[1:]))

    # ------------------------------------------------------------ operands
    def resolve(self, ref):
        """ref -> (base expression kind, text, inherited swizzle). Handles aliases, uniforms, consts, regs."""
        neg = ref.startswith("-")
        if neg:
            ref = ref[1:].strip()
        m = re.fullmatch(r"(\w+)(\[[^\]]*\])?(?:\.(\w+))?", ref)
        if not m:
            raise Err("bad operand: " + ref)
        base, idx, swz = m.group(1), m.group(2), m.group(3)
        inherited = None
        while base in self.aliases and not idx:
            tgt, aswz = self.aliases[base]
            m2 = re.fullmatch(r"(\w+)(\[[^\]]*\])?", tgt)
            base, idx = m2.group(1), m2.group(2)
            if aswz:
                inherited = aswz if inherited is None else "".join(pad_swz(aswz)[COMPS.index(c)] for c in pad_swz(inherited))
        if swz and inherited:
            sw = pad_swz(inherited)
            swz = "".join(sw[COMPS.index(c)] for c in pad_swz(swz.replace("r", "x").replace("g", "y").replace("b", "z").replace("a", "w"))[: len(swz)])
        elif inherited and not swz:
            swz = inherited
        return neg, base, idx, swz

    def src_expr(self, ref, lang):
        neg, base, idx, swz = self.resolve(ref)
        if base in self.consts:
            v = self.consts[base]
            t = "vec4" if lang == "glsl" else "float4"
            e = "%s(%s)" % (t, ", ".join(repr(float(x)) for x in v))
        elif base in self.ureg:
            ub, n = self.ureg[base]
            if idx:
                inner = idx[1:-1].replace(" ", "")
                mm = re.fullmatch(r"(?:a0\.([xy]))?\+?(-?\d+)?", inner)
                if not mm or (mm.group(1) is None and mm.group(2) is None):
                    raise Err("bad index: " + idx)
                off = ub + int(mm.group(2) or 0)
                if mm.group(1):
                    self.uses_a0 = True
                    e = "c[a0%s + %d]" % (mm.group(1), off)
                else:
                    e = "c[%d]" % off
            else:
                e = "c[%d]" % ub
        elif re.fullmatch(r"v\d+", base):
            self.inputs.add(int(base[1:]))
            e = "a%s" % base[1:]
        elif re.fullmatch(r"r\d+", base):
            self.temps.add(int(base[1:]))
            e = base
        elif base in self.outs:
            e = "o_" + base
        else:
            raise Err("unknown operand: " + ref)
        return neg, e, pad_swz(swz)

    def dest(self, ref):
        m = re.fullmatch(r"(\w+)(?:\.(\w+))?", ref)
        base, mask = m.group(1), m.group(2) or "xyzw"
        while base in self.aliases:
            base = self.aliases[base][0]
        if re.fullmatch(r"r\d+", base):
            self.temps.add(int(base[1:]))
            return base, mask
        if base in self.outs:
            return "o_" + base, mask
        if base == "a0":
            return "a0", mask
        raise Err("bad destination: " + ref)

    # ------------------------------------------------------------ code generation
    def gen(self, lang):
        V = (lambda n: "float" if n == 1 else "vec%d" % n) if lang == "glsl" else (lambda n: "float" if n == 1 else "float%d" % n)
        rsq = "inversesqrt" if lang == "glsl" else "rsqrt"
        stmts = []
        for op, d, srcs in self.body:
            dname, mask = self.dest(d)
            ops = [self.src_expr(s, lang) for s in srcs]

            def comp(i, positions):
                neg, e, sw = ops[i]
                s = "%s.%s" % (e, "".join(sw[p] for p in positions))
                return ("(-%s)" % s) if neg else s

            pos = [COMPS.index(c) for c in mask]
            if op == "mova":
                for c in mask:
                    neg, e, sw = ops[0]
                    v = "%s.%s" % (e, sw[COMPS.index(c)])
                    stmts.append("a0%s = int(%s%s);" % (c, "-" if neg else "", v))
                self.uses_a0 = True
                continue
            if op in ("dp3", "dp4", "rsq", "rcp"):
                if op == "dp3":
                    sc = "dot(%s, %s)" % (comp(0, [0, 1, 2]), comp(1, [0, 1, 2]))
                elif op == "dp4":
                    sc = "dot(%s, %s)" % (comp(0, [0, 1, 2, 3]), comp(1, [0, 1, 2, 3]))
                elif op == "rsq":
                    sc = "%s(%s)" % (rsq, comp(0, [0]))
                else:
                    sc = "(1.0 / %s)" % comp(0, [0])
                rhs = sc if len(mask) == 1 else "%s(%s)" % (V(len(mask)), sc)
            elif op == "mov":
                rhs = comp(0, pos)
            elif op in ("add", "mul"):
                rhs = "(%s %s %s)" % (comp(0, pos), "+" if op == "add" else "*", comp(1, pos))
            elif op in ("max", "min"):
                rhs = "%s(%s, %s)" % (op, comp(0, pos), comp(1, pos))
            elif op == "flr":
                rhs = "floor(%s)" % comp(0, pos)
            elif op in ("slt", "sge"):
                a, b = comp(0, pos), comp(1, pos)
                if lang == "glsl":
                    f = "lessThan" if op == "slt" else "greaterThanEqual"
                    rhs = ("%s(%s(%s, %s))" % (V(len(mask)), f, a, b)) if len(mask) > 1 else "float(%s %s %s)" % (a, "<" if op == "slt" else ">=", b)
                else:
                    rhs = "%s(%s %s %s)" % (V(len(mask)), a, "<" if op == "slt" else ">=", b)
            else:
                raise Err(op)
            stmts.append("%s.%s = %s;" % (dname, mask, rhs))

        v4 = "vec4" if lang == "glsl" else "float4"
        decl_temps = ["%s r%d = %s(0.0);" % (v4, t, v4) for t in sorted(self.temps)]
        decl_outs = ["%s o_%s = %s(0.0, 0.0, 0.0, 1.0);" % (v4, o, v4) for o in self.outs]
        if self.uses_a0:
            decl_outs.append("int a0x = 0; int a0y = 0;")
        pos_out = [o for o, s in self.outs.items() if s == "position"]
        if not pos_out:
            raise Err("no position output")
        po = "o_" + pos_out[0]
        tail = []
        varyings = []   # (name, glsl type, cg semantic, expr)
        for o, sem in self.outs.items():
            if sem.startswith("texcoord"):
                n = int(sem[len("texcoord"):] or 0)
                varyings.append(("vtc%d" % n, 2, "TEXCOORD%d" % n, "o_%s.xy" % o))
            elif sem == "color":
                varyings.append(("vcol", 4, "COLOR", "o_%s" % o))
            elif sem != "position":
                raise Err("unsupported output semantic " + sem)
        ninputs = max(self.inputs) + 1 if self.inputs else 0
        if lang == "glsl":
            lines = ["#version 100", "precision highp float;"]
            lines += ["attribute vec4 a%d;" % i for i in range(ninputs)]
            lines.append("uniform vec4 c[%d];" % max(1, self.nregs))
            lines += ["varying %s %s;" % (V(n), nm) for nm, n, _, _ in varyings]
            lines.append("void main() {")
            body_pos = "gl_Position = vec4(%s.x, %s.y, -2.0 * %s.z - %s.w, %s.w);" % ((po,) * 5)
        else:
            params = ["float4 a%d" % i for i in range(ninputs)]
            params.append("uniform float4 c[%d]" % max(1, self.nregs))
            params.append("float4 out vgl_Position : POSITION")
            params += ["%s out %s : %s" % (V(n), nm, sem) for nm, n, sem, _ in varyings]
            lines = ["void main(%s) {" % ", ".join(params)]
            body_pos = "vgl_Position = float4(%s.x, %s.y, -2.0 * %s.z - %s.w, %s.w);" % ((po,) * 5)
        lines += ["  " + l for l in decl_temps + decl_outs + stmts]
        lines.append("  " + body_pos)
        lines += ["  %s = %s;" % (nm, ex) for nm, _, _, ex in varyings]
        lines.append("}")
        out_mask = 0
        for nm, _, _, _ in varyings:
            if nm.startswith("vtc"):
                out_mask |= 1 << int(nm[3:])
            if nm == "vcol":
                out_mask |= 1 << 8
        return "\n".join(lines) + "\n", ninputs, out_mask


def c_string(s):
    return "\n".join('    "%s\\n"' % l.replace("\\", "\\\\").replace('"', '\\"') for l in s.rstrip("\n").split("\n"))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, outdir = sys.argv[1], sys.argv[2]
    name = os.path.basename(src)
    name = name[: -len(".v.pica")] if name.endswith(".v.pica") else os.path.splitext(name)[0]
    t = Translator(name, open(src).read())
    try:
        t.parse()
        glsl, nin, omask = t.gen("glsl")
        cg, nin2, _ = t.gen("cg")
    except Err as e:
        print("%s: %s" % (src, e), file=sys.stderr)
        return 1
    sid = sum(ord(ch) * 131 ** i for i, ch in enumerate(name)) & 0x7FFFFFFF
    uni = ", ".join('{%d, "%s"}' % (b, n) for n, b, _ in t.uniforms)
    h = []
    h.append("// %s_shbin.h - generated by platform/tools/pica2glsl.py from %s. Do not edit." % (name, os.path.basename(src)))
    h.append("#pragma once")
    h.append("#include <3ds.h>")
    h.append("static const char %s_glsl_src[] =\n%s;" % (name, c_string(glsl)))
    h.append("static const char %s_cg_src[] =\n%s;" % (name, c_string(cg)))
    h.append("static const CompatUniform %s_uniforms[] = {%s};" % (name, uni))
    h.append("#if defined(__vita__)")
    h.append("#define W3D_%s_SRC %s_cg_src" % (name.upper(), name))
    h.append("#else")
    h.append("#define W3D_%s_SRC %s_glsl_src" % (name.upper(), name))
    h.append("#endif")
    h.append("static DVLE_s %s_dvle = {W3D_%s_SRC, %d, %s_uniforms, %d, 0x%x, %d, 0x%x};"
             % (name, name.upper(), len(t.uniforms), name, nin, omask, max(1, t.nregs), sid))
    h.append("static DVLB_s %s_dvlb = {1, &%s_dvle};" % (name, name))
    h.append("// Same names as picasso + bin2s: DVLB_ParseFile recognises the marker size and returns the DVLB above.")
    h.append("static const u8* const %s_shbin = (const u8*)&%s_dvlb;" % (name, name))
    h.append("static const u32 %s_shbin_size = 0xC0DE5A7Eu;" % name)
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, name + "_shbin.h"), "w") as f:
        f.write("\n".join(h) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
