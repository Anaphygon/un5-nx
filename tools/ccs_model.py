#!/usr/bin/env python3
"""Exports the models of a UN5 CCS file, posed in their bind pose, as OBJ.

  tools/ccs_model.py <file.CCS> <out dir> [clump name filter]

Writes <name>.obj, <name>.mtl and one PNG per texture/palette pair. What it
writes comes from your disc: keep it on your machine.

The layout, as read from UN5 (SLES-55605); see tools/ccs_dump.py for chunks:
- clump 0x0900: id, node count, node object ids, then per node a local
  transform: position, rotation (degrees, X then Y then Z), scale.
- object 0x0100: id, parent object, model, shadow model, root.
- model 0x0800: id, scale, type, primitive count, draw flags, u16, lookup
  count (u8 of a word), two UN5 words, then for skinned models (type bit 2)
  a byte list mapping primitive bones to clump nodes, padded to 4.
  Primitives are triangle strips: a vertex whose normal's 4th byte is 0
  closes a triangle with the two before it.
    rigid: parent, material, n, int16 xyz (padded to 4), normals,
      colours (unless type bit 0x400), uvs (unless a 0x600 morph target).
    skinned model, one bone: material, n, 0, bone, xyz, normals, uvs.
    skinned model, weighted: material, n, entries, then per entry int16
      xyz in its joint's space + u16 (weight: low 9 bits of 256, bit 9: the
      vertex's last entry, joint: bits 10+), per entry normals, per vertex uvs.
- material 0x0200: id, texture id, alpha, uv offset.
- texture 0x0300: id, clut id, u32, u8 flags, u8 type (0x13 I8, 0x14 I4),
  u8 mips, u8, u8 log2 width, u8 log2 height, u16, u32, data words, data.
- clut 0x0400: id, u32 x3, colour count, RGBA (alpha 0x80 = opaque).
"""
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ccs_dump import a4, chunks, index_names, load  # noqa: E402

VERTEX_UNIT = 1 / 256 / 16      # int16 vertex * model scale * this = clump units


def strip_triangles(flags: list[int]) -> list[tuple[int, int, int]]:
    tris = []
    for i, f in enumerate(flags):
        if f == 0 and i >= 2:
            tris.append((i - 2, i - 1, i) if i % 2 == 0 else (i - 1, i - 2, i))
    return tris


def read_model(u: bytes, p: int) -> dict:
    mid, scale, typ, nprim, _, _, gif = struct.unpack_from('<IfHHHHI', u, p)
    model = {'id': mid, 'scale': scale, 'type': typ, 'lookup': [], 'prims': []}
    q = p + 28
    s = scale * VERTEX_UNIT
    if typ & 2 or (typ & 8):        # rare variant / shadow volumes: not drawn here
        return model
    if typ & 4:
        model['lookup'] = list(u[q:q + (gif & 0xFF)])
        q += a4(gif & 0xFF)
        for _ in range(nprim):
            mat, n, entries = struct.unpack_from('<3I', u, q)
            if entries == 0:
                bone = struct.unpack_from('<I', u, q + 12)[0]
                q += 16
                xyz = [struct.unpack_from('<3h', u, q + i * 6) for i in range(n)]
                q = a4(q + n * 6)
                flags = [u[q + i * 4 + 3] for i in range(n)]
                q += n * 4
                uv = [struct.unpack_from('<2h', u, q + i * 4) for i in range(n)]
                q += n * 4
                verts = [[(bone, 256, (x * s, y * s, z * s))] for x, y, z in xyz]
            else:
                q += 12
                raw = [struct.unpack_from('<3hH', u, q + i * 8) for i in range(entries)]
                q += entries * 8
                nflags = [u[q + i * 4 + 3] for i in range(entries)]
                q += entries * 4
                uv = [struct.unpack_from('<2h', u, q + i * 4) for i in range(n)]
                q += n * 4
                verts, flags, cur = [], [], []
                for (x, y, z, j), f in zip(raw, nflags):
                    cur.append((j >> 10, j & 0x1FF, (x * s, y * s, z * s)))
                    if j & 0x200:
                        verts.append(cur)
                        flags.append(f)
                        cur = []
            model['prims'].append({'mat': mat, 'verts': verts, 'uv': uv, 'flags': flags})
        return model
    for _ in range(nprim):
        parent, mat, n = struct.unpack_from('<3I', u, q)
        q += 12
        xyz = [struct.unpack_from('<3h', u, q + i * 6) for i in range(n)]
        q = a4(q + n * 6)
        flags = [u[q + i * 4 + 3] for i in range(n)]
        q += n * 4
        if typ & 0x600 == 0x600:
            continue
        if not typ & 0x400:
            q += n * 4
        uv = [struct.unpack_from('<2h', u, q + i * 4) for i in range(n)]
        q += n * 4
        verts = [[(None, 256, (x * s, y * s, z * s))] for x, y, z in xyz]
        model['prims'].append({'mat': mat, 'verts': verts, 'uv': uv, 'flags': flags})
    return model


def parse(u: bytes) -> dict:
    ccs = {'names': [], 'objects': {}, 'models': {}, 'materials': {}, 'textures': {}, 'cluts': {}, 'clumps': []}
    for pos, t, nbytes in chunks(u):
        p = pos + 8
        size = nbytes // 4
        if t == 0x0002:
            ccs['names'] = index_names(u, p)
        elif t == 0x0100:
            w = struct.unpack_from('<%dI' % min(size, 5), u, p)
            ccs['objects'][w[0]] = {'parent': w[1], 'model': w[2]}
        elif t == 0x0900:
            cid, n = struct.unpack_from('<II', u, p)
            if size >= 2 + n * 10:
                ids = struct.unpack_from('<%dI' % n, u, p + 8)
                pose = [struct.unpack_from('<9f', u, p + 8 + n * 4 + i * 36) for i in range(n)]
                ccs['clumps'].append({'id': cid, 'nodes': list(ids), 'pose': pose})
        elif t == 0x0200:
            mid, tex = struct.unpack_from('<II', u, p)
            ccs['materials'][mid] = tex
        elif t == 0x0300:
            tid, clut = struct.unpack_from('<II', u, p)
            ttype, wl, hl = u[p + 13], u[p + 16], u[p + 17]
            words = struct.unpack_from('<I', u, p + 24)[0]
            ccs['textures'][tid] = {'clut': clut, 'type': ttype, 'w': 1 << wl, 'h': 1 << hl, 'data': u[p + 28:p + 28 + words * 4]}
        elif t == 0x0400:
            cid, _, _, _, n = struct.unpack_from('<5I', u, p)
            ccs['cluts'][cid] = [u[p + 20 + i * 4:p + 24 + i * 4] for i in range(n)]
        elif t == 0x0800:
            ccs['models'][struct.unpack_from('<I', u, p)[0]] = read_model(u, p)
    return ccs


# 4x4 matrices as row-major lists of 16 floats, column vectors.
def mat_mul(a, b):
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4)) for r in range(4) for c in range(4)]


def local_matrix(pose):
    px, py, pz, rx, ry, rz, sx, sy, sz = pose
    cx, sx_ = math.cos(math.radians(rx)), math.sin(math.radians(rx))
    cy, sy_ = math.cos(math.radians(ry)), math.sin(math.radians(ry))
    cz, sz_ = math.cos(math.radians(rz)), math.sin(math.radians(rz))
    mx = [1, 0, 0, 0, 0, cx, -sx_, 0, 0, sx_, cx, 0, 0, 0, 0, 1]
    my = [cy, 0, sy_, 0, 0, 1, 0, 0, -sy_, 0, cy, 0, 0, 0, 0, 1]
    mz = [cz, -sz_, 0, 0, sz_, cz, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    r = mat_mul(mz, mat_mul(my, mx))
    m = [r[0] * sx, r[1] * sy, r[2] * sz, px, r[4] * sx, r[5] * sy, r[6] * sz, py, r[8] * sx, r[9] * sy, r[10] * sz, pz, 0, 0, 0, 1]
    return m


def apply(m, v):
    x, y, z = v
    return (m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7], m[8] * x + m[9] * y + m[10] * z + m[11])


def world_matrices(ccs: dict, clump: dict) -> dict:
    local = dict(zip(clump['nodes'], (local_matrix(p) for p in clump['pose'])))
    world: dict = {}

    def get(oid):
        if oid in world:
            return world[oid]
        parent = ccs['objects'].get(oid, {}).get('parent', 0)
        m = local.get(oid, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        world[oid] = mat_mul(get(parent), m) if parent in local and parent != oid else m
        return world[oid]

    for oid in clump['nodes']:
        get(oid)
    return world


def texture_rgba(ccs: dict, tid: int) -> tuple[int, int, bytes] | None:
    tex = ccs['textures'].get(tid)
    if not tex or tex['clut'] not in ccs['cluts'] or tex['type'] not in (0x13, 0x14):
        return None
    cols = ccs['cluts'][tex['clut']]
    if len(cols) == 256:    # PS2 CSM1 order: entries 8-15 and 16-23 of every 32 swapped
        cols = [cols[(i & ~0x18) | ((i & 8) << 1) | ((i & 16) >> 1)] for i in range(256)]
    w, h, d = tex['w'], tex['h'], tex['data']
    idx = d[:w * h] if tex['type'] == 0x13 else bytes(v for b in d[:w * h // 2] for v in (b & 15, b >> 4))
    out = bytearray()
    for i in idx:
        r, g, b, a = cols[i] if i < len(cols) else b'\0\0\0\0'
        out += bytes((r, g, b, min(255, a * 2)))
    return w, h, bytes(out)


def write_png(path: str, w: int, h: int, rgba: bytes) -> None:
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    rows = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                            + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def export_clump(ccs: dict, clump: dict, out: str) -> str:
    names = ccs['names']
    base = names[clump['id']].replace(' ', '_') or 'clump%d' % clump['id']
    world = world_matrices(ccs, clump)
    obj, mtl, used = [], [], set()
    nv = 0
    for oid in clump['nodes']:
        model = ccs['models'].get(ccs['objects'].get(oid, {}).get('model'))
        if not model or not model['prims']:
            continue
        obj.append('o %s' % names[oid].replace(' ', '_'))
        for prim in model['prims']:
            tid = ccs['materials'].get(prim['mat'])
            if tid is not None and tid not in used:
                used.add(tid)
                img = texture_rgba(ccs, tid)
                if img:
                    write_png(os.path.join(out, 'tex%d.png' % tid), *img)
                mtl += ['newmtl tex%d' % tid, 'map_Kd tex%d.png' % tid, '']
            if tid is not None:
                obj.append('usemtl tex%d' % tid)
            for influences, (s, t) in zip(prim['verts'], prim['uv']):
                p = [0.0, 0.0, 0.0]
                for joint, weight, v in influences:
                    node = clump['nodes'][model['lookup'][joint]] if joint is not None else oid
                    wv = apply(world.get(node, world[oid]), v)
                    p = [a + b * weight / 256 for a, b in zip(p, wv)]
                obj.append('v %.4f %.4f %.4f' % tuple(p))
                obj.append('vt %.5f %.5f' % (s / 256, 1 - t / 256))
            for a, b, c in strip_triangles(prim['flags']):
                obj.append('f %d/%d %d/%d %d/%d' % (nv + a + 1, nv + a + 1, nv + b + 1, nv + b + 1, nv + c + 1, nv + c + 1))
            nv += len(prim['verts'])
    open(os.path.join(out, base + '.mtl'), 'w').write('\n'.join(mtl))
    open(os.path.join(out, base + '.obj'), 'w').write('mtllib %s.mtl\n' % base + '\n'.join(obj) + '\n')
    return base


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ccs = parse(load(sys.argv[1]))
    os.makedirs(sys.argv[2], exist_ok=True)
    want = sys.argv[3] if len(sys.argv) > 3 else ''
    for clump in ccs['clumps']:
        if want in ccs['names'][clump['id']]:
            print(export_clump(ccs, clump, sys.argv[2]))


if __name__ == '__main__':
    main()
