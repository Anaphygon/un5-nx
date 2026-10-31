#!/usr/bin/env python3
"""Plays a UN5 CCS animation on a CCS model and exports chosen frames as OBJ.

  tools/ccs_anim.py <anim.CCS> <model.CCS> <out dir> [animation [frame ...]]

Without an animation name it lists the file's animations. What it writes
comes from your disc: keep it on your machine.

An anime chunk (0x0700) holds id, frame count, size in words, then
sub-chunks: 0xFF01 frame markers (the frame number, -1 to end) and per
object controllers, 0x0102 for objects (0x0202 materials, 0x0108 effects):
object id, track mask, then the tracks in order position, rotation, scale,
alpha. The mask gives 3 bits per track: 0 absent, 1 one value, 2 keys of
(frame, xyz), 4 keys of (frame, quaternion xyzw, inverse as 3ds Max
stores it); alpha values are single floats. Fixed and xyz rotations are
degrees, composed as in ccs_model.euler_matrix.

Battle motions key a shared skeleton (OBJ_1cmn00t0 <bone>, reached through
external chunks); a character's model names the same bones under its own
prefix (OBJ_2ank00t0 <bone>), so tracks are matched by bone name.
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ccs_model  # noqa: E402
from ccs_dump import chunks, index_names, load  # noqa: E402

TRACKS = ('pos', 'rot', 'scale', 'alpha')


def bone_name(name: str) -> str:
    """'OBJ_1cmn00t0 l thigh' -> 'l thigh'; a skeleton's root object -> ''."""
    rest = name[4:] if name.startswith('OBJ_') else name
    return rest.split(' ', 1)[1] if ' ' in rest else ''


def read_track(u: bytes, q: int, kind: int, width: int) -> tuple[list, int]:
    """Keys [(frame, values)] of one track and the position after it."""
    if kind == 1:
        return [(0, struct.unpack_from('<%df' % width, u, q))], q + width * 4
    if kind in (2, 4):
        values = 4 if kind == 4 else width
        n = struct.unpack_from('<I', u, q)[0]
        q += 4
        keys = []
        for _ in range(n):
            frame = struct.unpack_from('<i', u, q)[0]
            keys.append((frame, struct.unpack_from('<%df' % values, u, q + 4)))
            q += 4 + values * 4
        return keys, q
    raise ValueError('track kind %d' % kind)


def parse_animes(u: bytes) -> dict:
    """{name: {'frames': n, 'objects': {object name: {track: (kind, keys)}}}}"""
    names: list[str] = []
    animes = {}
    for pos, t, _ in chunks(u):
        if t == 0x0002:
            names = index_names(u, pos + 8)
        if t != 0x0700:
            continue
        aid, frames, size = struct.unpack_from('<3I', u, pos + 8)
        objects: dict = {}
        q, end = pos + 20, pos + 20 + size * 4
        while q < end:
            st, ss = struct.unpack_from('<II', u, q)
            if st & 0xFFFF == 0x0102:
                obj, mask = struct.unpack_from('<2I', u, q + 8)
                r, tracks = q + 16, {}
                for i, track in enumerate(TRACKS):
                    kind = (mask >> (3 * i)) & 7
                    if kind:
                        keys, r = read_track(u, r, kind, 1 if track == 'alpha' else 3)
                        tracks[track] = (kind, keys)
                objects[names[obj]] = tracks
            q += 8 + ss * 4
        animes[names[aid]] = {'frames': frames, 'objects': objects}
    return animes


def sample(kind: int, keys: list, frame: float) -> tuple:
    if len(keys) == 1 or frame <= keys[0][0]:
        return keys[0][1]
    for (f0, v0), (f1, v1) in zip(keys, keys[1:]):
        if f0 <= frame <= f1:
            t = (frame - f0) / (f1 - f0) if f1 != f0 else 0.0
            if kind == 4 and sum(a * b for a, b in zip(v0, v1)) < 0:
                v1 = tuple(-x for x in v1)      # shortest way round
            return tuple(a + (b - a) * t for a, b in zip(v0, v1))   # normalised later
    return keys[-1][1]


def pose_locals(ccs: dict, clump: dict, anime: dict, frame: float) -> dict:
    """Local matrices of the clump's nodes at a frame: the animation's
    tracks where it keys the bone, the bind pose elsewhere."""
    by_bone = {bone_name(name): tracks for name, tracks in anime['objects'].items()}
    local = {}
    for oid, bind in zip(clump['nodes'], clump['pose']):
        tracks = by_bone.get(bone_name(ccs['names'][oid]), {})
        pos = sample(*tracks['pos'], frame) if 'pos' in tracks else bind[0:3]
        scale = sample(*tracks['scale'], frame) if 'scale' in tracks else bind[6:9]
        if 'rot' in tracks:
            kind, keys = tracks['rot']
            v = sample(kind, keys, frame)
            rot = ccs_model.quat_matrix(*v) if kind == 4 else ccs_model.euler_matrix(*v)
        else:
            rot = ccs_model.euler_matrix(*bind[3:6])
        local[oid] = ccs_model.trs_matrix(pos, rot, scale)
    return local


def main() -> None:
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    animes = parse_animes(load(sys.argv[1]))
    if len(sys.argv) < 5:
        for name, a in animes.items():
            print('%-28s %4d frames %3d objects' % (name, a['frames'], len(a['objects'])))
        return
    anime = animes[sys.argv[4]]
    frames = [float(f) for f in sys.argv[5:]] or [0.0]
    ccs = ccs_model.parse(load(sys.argv[2]))
    clump = max(ccs['clumps'], key=lambda c: len(c['nodes']))
    os.makedirs(sys.argv[3], exist_ok=True)
    for frame in frames:
        local = pose_locals(ccs, clump, anime, frame)
        print(ccs_model.export_clump(ccs, clump, sys.argv[3], local, '_%s_f%03d' % (sys.argv[4][4:], math.floor(frame))))


if __name__ == '__main__':
    main()
