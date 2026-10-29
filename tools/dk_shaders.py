#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""Build the deko3d shader pack from harvested GLSL.

The OpenGL GS device on the Switch writes every program it compiles to
sdmc:/switch/un5-nx/shaders-gl/<hash>.{vert,frag} (and the deko3d device writes
the ones its pack lacks to shaders-dk-missing/). This compiles each with
devkitPro's uam into <hash>.dksh, the name the deko3d device looks up.

uam takes PCSX2's OpenGL GLSL as it is, except for uniforms declared outside a
block. Those are gathered, after preprocessing, into one std140 block at
binding 7, in declaration order; GSDeviceDK fills that block with the same
layout.

    tools/dk_shaders.py <harvest dir>... -o <pack dir>
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

UAM = os.environ.get("UAM", "/opt/devkitpro/tools/bin/uam")
LOOSE_BINDING = 7
LOOSE = re.compile(r"^[ \t]*uniform[ \t]+(?!sampler|isampler|usampler|image|iimage|uimage)(\w+)[ \t]+(\w+)[ \t]*;[ \t]*$", re.M)


def preprocess(source):
    """Runs the C preprocessor over the shader, keeping #version/#extension."""
    keep, body = [], []
    for line in source.splitlines():
        (keep if line.lstrip().startswith(("#version", "#extension")) else body).append(line)
    out = subprocess.run(["clang", "-E", "-P", "-x", "c", "-w", "-"], input="\n".join(body),
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError(out.stderr)
    return "\n".join(keep) + "\n" + out.stdout


def gather_loose_uniforms(source):
    """Puts uniforms declared outside a block into one std140 block."""
    if not LOOSE.search(source):
        return source
    flat = preprocess(source)
    members = LOOSE.findall(flat)
    if not members:
        return source
    first = LOOSE.search(flat).start()
    flat = LOOSE.sub("", flat)
    block = "layout(std140, binding = %d) uniform un5_loose\n{\n%s};\n" % (
        LOOSE_BINDING, "".join("\t%s %s;\n" % m for m in members))
    return flat[:first] + block + flat[first:]


def compile_one(path, out_dir):
    name, ext = os.path.splitext(os.path.basename(path))
    stage = {".vert": "vert", ".frag": "frag"}.get(ext)
    if not stage:
        return None
    out = os.path.join(out_dir, name + ".dksh")
    if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(path):
        return "cached"
    with open(path, encoding="utf-8", errors="replace") as f:
        source = gather_loose_uniforms(f.read())
    with tempfile.NamedTemporaryFile("w", suffix=".glsl", delete=False) as tmp:
        tmp.write(source)
    try:
        res = subprocess.run([UAM, "-s", stage, "-o", out, tmp.name], capture_output=True, text=True)
    finally:
        os.unlink(tmp.name)
    if res.returncode != 0:
        if os.path.exists(out):
            os.unlink(out)
        sys.stderr.write("FAIL %s: %s\n" % (path, (res.stdout + res.stderr).strip()[:400]))
        return "failed"
    return "built"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("inputs", nargs="+", help="directories of harvested .vert/.frag files")
    ap.add_argument("-o", "--out", required=True, help="pack directory")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    counts = {}
    for d in args.inputs:
        for entry in sorted(os.listdir(d)):
            result = compile_one(os.path.join(d, entry), args.out)
            if result:
                counts[result] = counts.get(result, 0) + 1
    print(", ".join("%d %s" % (n, k) for k, n in sorted(counts.items())) or "nothing to do")
    return 1 if counts.get("failed") else 0


if __name__ == "__main__":
    sys.exit(main())
