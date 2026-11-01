// SPDX-License-Identifier: GPL-3.0+
// Native stand-ins for UN5's hottest VU1 programs (see docs/VU1-NATIVE.md and
// ps2-web-recomp's register_native). Each one does what the translated
// program does, in the same order of float operations and in the same
// round-toward-zero mode, so the results are bit-identical; it only drops
// what translating one VU instruction at a time costs: registers kept in
// memory, vfix after every operation and the flag pipeline. Cases it does not
// cover go to the translated program (vu::native_fallback()).
//
// Checked with PS2_VU_NATIVE_CHECK=1 on the Mac battle replay: every call is
// run both ways and compared (DC7A: 169,584 calls, 0 differ). On the Mac the
// gain is small (1.02-1.08x per call): clang's translation is already close
// to the arithmetic. The Switch build (GCC) is timed with PS2_VU_NATIVE=alt.
#include "ps2/vu_ops.hpp"

#include "ps2/fpu.hpp"

#include <cstring>

namespace {

using namespace ps2;
using namespace ps2::vuops;

constexpr std::uint32_t kOne = 0x3F800000u;

v4u load(const std::uint8_t *mem, std::uint32_t index) {
    v4u v;
    std::memcpy(&v, qword(const_cast<std::uint8_t *>(mem), index), 16);
    return v;
}
void store(std::uint8_t *mem, std::uint32_t index, v4u v) { std::memcpy(qword(mem, index), &v, 16); }
void store_xyz(std::uint8_t *mem, std::uint32_t index, v4u v) { std::memcpy(qword(mem, index), &v, 12); }
void store_w(std::uint8_t *mem, std::uint32_t index, std::uint32_t w) { std::memcpy(qword(mem, index) + 12, &w, 4); }
v4u with_xyz(v4u old, v4u now) { return vsel(old, now, v4u{~0u, ~0u, ~0u, 0u}); }
v4u with_w(v4u old, v4u now) { return vsel(old, now, v4u{0u, 0u, 0u, ~0u}); }
// Sign bits of the x, y and w lanes as MAC flags (x 0x80, y 0x40, w 0x10).
std::uint16_t signs_xyw(v4u x) {
    return static_cast<std::uint16_t>(((x[0] >> 24) & 0x80u) | ((x[1] >> 25) & 0x40u) | ((x[3] >> 27) & 0x10u));
}

// DC7A318002C3DD86, family A (UN5's stage and other rigid, vertex-coloured
// meshes; entry 0x50, continued by MSCNT). One batch: a GIF tag in the first
// qword at TOP (vertex count in its low 15 bits), then per vertex 4 qwords:
// position (int xyz, 12 fraction bits; strip flags in w), colour (ints),
// texture coordinates (int xy, 12 fraction bits; z = 1.0 from the row).
// Per vertex: the matrix in VF1-VF4, Q = 1 / w, fog = VF15.z + w * VF15.w
// clamped to VF15.x..y, STQ, screen XYZ in 12.4, colour with its alpha scaled
// by VF16.w; the output (fog, ST, RGBA, XYZ2) goes to TOP + 202 on, after a
// copy of the tag. The ADC bit drops triangles whose three vertices are not
// all inside the guard band VF13..VF14. A triangle that is not, and has a
// vertex behind the near plane (w < VF16.x), needs clipping: that batch is
// left to the translated program, which has the clipper.
void dc7a(Vu0Regs &r, std::uint8_t *mem) {
    const v4u n1 = vfix(vload(r.vf[1])), n2 = vfix(vload(r.vf[2])), n3 = vfix(vload(r.vf[3])), n4 = vfix(vload(r.vf[4]));
    const v4u n13 = vfix(vload(r.vf[13])), n14 = vfix(vload(r.vf[14])), n15 = vfix(vload(r.vf[15])), n16 = vfix(vload(r.vf[16]));
    const v4u f17 = vload(r.vf[17]);
    const std::uint32_t top = vu::top();
    // L_0050: the previous batch's last colour qword (the clipper's), first.
    store(mem, r.vi[1] + 7u, vload(r.vf[25]));
    const v4u header = load(mem, top);
    const std::uint32_t count = header[0] & 0x7FFFu;
    // Find out first whether any triangle needs the clipper, without writing.
    {
        std::uint16_t i7 = r.vi[7], i8 = r.vi[8], i10 = r.vi[10];
        for (std::uint32_t k = 0; k < count; ++k) {
            const std::uint32_t base = top + 9 + 4 * k;
            const v4u in0 = load(mem, base);
            const v4u p = vitof(in0, 12);
            v4u acc = vmulz(n1, vsplat(p[0]));
            acc = vmaddz(acc, n2, vsplat(p[1]));
            acc = vmaddz(acc, n3, vsplat(p[2]));
            const v4u clip = vmaddz(acc, n4, vsplat(kOne));
            const std::uint32_t q = fp::div(kOne, clip[3]).value;
            const v4u screen = with_xyz(clip, vmulz(clip, vfix(vsplat(q))));
            const std::uint16_t in_a = signs_xyw(vsubz(n13, screen));
            const std::uint16_t in_b = signs_xyw(vsubz(screen, n14));
            const std::uint16_t i1 = static_cast<std::uint16_t>(in_a & 208u & in_b);
            const std::uint16_t flag = static_cast<std::uint16_t>(in0[3]);
            i10 = i1 != 208u ? static_cast<std::uint16_t>(i10 - 1u) : std::uint16_t{2};
            const std::uint16_t i11 = static_cast<std::uint16_t>(static_cast<std::uint16_t>(flag + 32767u) | i10);
            const std::uint16_t i9 = static_cast<std::uint16_t>(i8 & i1);
            i8 = static_cast<std::uint16_t>(i7 & i1);
            i7 = i1;
            if (i9 == 208u || static_cast<std::int16_t>(i11) < 0) continue;
            const v4u near = vsubz(clip, vsplat(n16[0]));
            if (near[3] & 0x80000000u) {
                vu::native_fallback()(r, mem);
                return;
            }
        }
    }

    // The tag (L_0060..L_0098).
    v4u f18 = vload(r.vf[18]);
    v4u f8 = vload(r.vf[8]);
    f18 = v4u{header[0], f17[1], f17[2], header[3]};
    f8 = v4u{header[0], f8[1], f17[2], header[3]};
    std::uint32_t i4 = top + 201u;
    const std::uint32_t i5 = i4;
    store(mem, i4, f18);
    i4 += 1u;
    std::uint16_t i3 = static_cast<std::uint16_t>(count);
    std::uint16_t i7 = r.vi[7], i8 = r.vi[8], i9 = r.vi[9], i10 = r.vi[10], i11 = r.vi[11], i12 = r.vi[12];
    std::uint16_t i1 = 0, i6 = 0;
    std::uint32_t i2 = top + 9u;

    // The first vertex's loads and transform (L_00A0..L_0100).
    v4u f22 = vload(r.vf[22]);
    f22[3] = 0u;
    v4u f23 = load(mem, i2), f24 = load(mem, i2 + 2u), f25 = load(mem, i2 + 1u);
    v4u f20 = with_xyz(vload(r.vf[20]), vitof(f23, 12));
    f22 = vsel(f22, vitof(f24, 12), v4u{~0u, ~0u, 0u, 0u});
    v4u f21 = vitof(f25, 0);
    f22[2] = vaddz(vfix(f24), v4u{0u, 0u, 0u, kOne})[2];
    v4u acc = vmulz(n1, vsplat(f20[0]));
    acc = vmaddz(acc, n2, vsplat(f20[1]));
    acc = vmaddz(acc, n3, vsplat(f20[2]));
    f20 = vmaddz(acc, n4, vsplat(kOne));
    f21[3] = vmulz(f21, n16)[3];
    acc[3] = vmulz(v4u{0u, 0u, 0u, kOne}, vsplat(n15[2]))[3];
    std::uint32_t q = r.q;

    do {
        // L_0108..L_0210 for the vertex at i2.
        f18[3] = vmaddz(acc, f20, vsplat(n15[3]))[3];
        q = fp::div(kOne, f20[3]).value;
        i12 = static_cast<std::uint16_t>(load(mem, i2)[3]);
        store(mem, i2, f20);
        store(mem, i2 + 1u, f21);
        store(mem, i2 + 2u, f22);
        f18 = with_w(f18, vmax(f18, vsplat(r.vf[15].w[0])));
        i11 = static_cast<std::uint16_t>(i12 + 32767u);
        i12 = static_cast<std::uint16_t>(i12 - 32561u);
        const v4u vq = vfix(vsplat(q));
        f20 = with_xyz(f20, vmulz(f20, vq));
        f23 = vsel(f23, load(mem, i2 + 4u), v4u{~0u, ~0u, ~0u, 0u});
        f22 = with_xyz(f22, vmulz(f22, vq));
        f24 = vsel(f24, load(mem, i2 + 6u), v4u{~0u, ~0u, ~0u, 0u});
        f18 = with_w(f18, vmin(f18, vsplat(r.vf[15].w[1])));
        f25 = load(mem, i2 + 5u);
        f21 = vftoi(f21, 0);
        i3 = static_cast<std::uint16_t>(i3 - 1u);
        const std::uint16_t mac_a = signs_xyw(vsubz(n13, f20));
        i2 += 4u;
        const std::uint16_t mac_b = signs_xyw(vsubz(f20, n14));
        i4 += 4u;
        f20 = with_xyz(f20, vftoi(f20, 4));
        store_xyz(mem, i4 - 3u, f22);
        f18 = with_w(f18, vftoi(f18, 4));
        store(mem, i4 - 2u, f21);
        f22 = vsel(f22, vitof(f24, 12), v4u{~0u, ~0u, 0u, 0u});
        i1 = static_cast<std::uint16_t>(mac_a & 208u);
        f21 = vitof(f25, 0);
        i1 = static_cast<std::uint16_t>(mac_b & i1);
        f22[2] = vaddz(vfix(f24), v4u{0u, 0u, 0u, kOne})[2];
        store_xyz(mem, i4 - 1u, f20);
        const v4u w_now = f20;  // w: this vertex's clip w, for the near test
        f20 = with_xyz(f20, vitof(f23, 12));
        i10 = static_cast<std::uint16_t>(i10 - 1u);
        if (i1 == 208u) i10 = 2u;
        i11 = static_cast<std::uint16_t>(i11 | i10);
        i9 = static_cast<std::uint16_t>(i8 & i1);
        acc = vmulz(n1, vsplat(f20[0]));
        i8 = static_cast<std::uint16_t>(i7 & i1);
        acc = vmaddz(acc, n2, vsplat(f20[1]));
        i7 = i1;
        const bool behind = (vsubz(w_now, vsplat(n16[0]))[3] & 0x80000000u) != 0;
        i1 = static_cast<std::uint16_t>(i12 - i9);
        acc = vmaddz(acc, n3, vsplat(f20[2]));
        f20 = vmaddz(acc, n4, vsplat(kOne));
        store_w(mem, i4 - 1u, i1);
        if (i9 != 208u && static_cast<std::int16_t>(i11) >= 0) {
            i1 = behind ? 2u : 0u;
            store_w(mem, i2 - 2u, i1);
            i6 = static_cast<std::uint16_t>(i6 | i1);
        } else if (i9 != 208u) {
            i1 = behind ? 2u : 0u;
        }
        f21[3] = vmulz(f21, n16)[3];
        acc[3] = vmulz(v4u{0u, 0u, 0u, kOne}, vsplat(n15[2]))[3];
        store_w(mem, i4 - 4u, f18[3]);
    } while (i3 != 0u);

    // L_0218..L_0278: keep the last two vertices for the next batch, then
    // send the packet (L_0040) and end at 0x50.
    i1 = static_cast<std::uint16_t>(vu::itop());
    f20 = load(mem, i2 - 8u);
    f21 = load(mem, i2 - 7u);
    f22 = load(mem, i2 - 6u);
    f23 = load(mem, i2 - 4u);
    f24 = load(mem, i2 - 3u);
    f25 = load(mem, i2 - 2u);
    store(mem, i1 + 1u, f20);
    store(mem, i1 + 2u, f21);
    store(mem, i1 + 3u, f22);
    store(mem, i1 + 5u, f23);
    store(mem, i1 + 6u, f24);
    vu::kick(1, i5);

    vstore(r.vf[8], f8);
    vstore(r.vf[18], f18);
    vstore(r.vf[20], f20);
    vstore(r.vf[21], f21);
    vstore(r.vf[22], f22);
    vstore(r.vf[23], f23);
    vstore(r.vf[24], f24);
    vstore(r.vf[25], f25);
    vstore(r.acc, acc);
    r.q = q;
    r.vi[1] = i1;
    r.vi[2] = static_cast<std::uint16_t>(i2);
    r.vi[3] = i3;
    r.vi[4] = static_cast<std::uint16_t>(i4);
    r.vi[5] = static_cast<std::uint16_t>(i5);
    r.vi[6] = i6;
    r.vi[7] = i7;
    r.vi[8] = i8;
    r.vi[9] = i9;
    r.vi[10] = i10;
    r.vi[11] = i11;
    r.vi[12] = i12;
    r.vi[13] = 208u;
    r.tpc = 0x50u;
}

struct Register {
    Register() { vu::register_native(0xDC7A318002C3DD86ull, dc7a); }
} g_register;

} // namespace
