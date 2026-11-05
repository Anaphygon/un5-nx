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
// to the arithmetic. On the Switch (GCC) it is 1.07x (13.9 vs 14.9 us per call)
// and PS2_VU_BENCH found its VU memory not identical there, so natives are
// opt-in (PS2_VU_NATIVE=1); GCC contracting float ops into FMAs is suspected.
//
// DC7A can also hand its batches to the GS untransformed (dc7a_batch, below):
// checked with UN5_STAGE_REF=1 on the Mac regression, where the registers it
// leaves match the program's on all 168,000 battle calls and the frames
// differ from the baselines by rounding only (battle 10/62 frames, at most
// 0.5% of pixels and 4 levels; adv 41/160, under 100 pixels; movie and shop
// identical).
#include "ps2/vu_ops.hpp"

#include "ps2/fpu.hpp"

#include <cstdlib>
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

// ---- DC7A on the GPU ----
//
// With a GS that draws these batches itself (un5-nx's PCSX2 build on the
// Switch, see gs-pcsx2/patches), the batch is not transformed here at all:
// its raw vertices and the constants it is transformed with go to the GS as
// one packet, in the place of the strip the program would have sent, and
// the GS's vertex shader does the rest. The packet is a PACKED GIF tag of
// NOP registers, which any other GS skips:
//
//   0      GIF tag: NLOOP = qwords that follow, EOP, PACKED, one NOP register
//   1      kBatchMagic, version, vertex count, uv z (Q is z / w, so the
//          texture coordinate is uv / z; one value for the whole batch)
//   2      the GIF tag the program would have sent (PRIM, the 512A registers)
//   3..6   VF1..VF4, the matrix: clip = VF1 x + VF2 y + VF3 z + VF4
//   7..10  VF13, VF14 (the guard band; w is the depth range), VF15 (fog:
//          clamp(z + w * clip w, x, y)), VF16 (x near plane, w alpha scale)
//   11..   a vertex per qword: x, y | z, strip flag | RGBA | u, v (int16,
//          12 fraction bits for position and uv)
//
// It is 11 + n qwords, where the strip was 1 + 4n, so it fits where the
// program writes its output. What the program leaves behind for the next
// batch (VI7..VI12, the last two vertices it keeps for its clipper, VF20..25)
// is worked out exactly from the last few vertices. Batches with values out
// of int16 range, or fewer than 4 vertices, go to the compiled program.
//
// UN5_STAGE_REF=1 (no such GS, on the Mac): the packet is built and then
// expanded back into the strip the program would have sent, with float math
// as the vertex shader does it, so the regression frames test the packet,
// the expansion and the state left behind. Batches that need the clipper
// run the compiled program there.

constexpr std::uint32_t kBatchMagic = 0x4E354E55u; // "UN5N"
constexpr std::uint32_t kBatchVersion = 1;
constexpr std::uint32_t kInside = 208u;           // x, y and w within the guard band

}

// Set by un5-nx's PCSX2 front end when it opens and can draw these packets
// (gs-pcsx2/src/gs_front.cpp); read from the first DC7A batch on.
extern "C" {
bool (*un5_native_batches_hook)() = nullptr;
}

namespace {

bool gpu_batches() {
    static const bool on = un5_native_batches_hook && un5_native_batches_hook();
    return on;
}

bool reference_batches() {
    static const bool on = [] {
        const char *e = std::getenv("UN5_STAGE_REF");
        return e && e[0] == '1';
    }();
    return on;
}

// One vertex as the program sees it, exactly: its clip position, whether it
// is inside the guard band and behind the near plane, and q.
struct Seen {
    v4u clip;
    std::uint16_t inside;
    bool behind;
    std::uint32_t q;
};

Seen see(const v4u &n1, const v4u &n2, const v4u &n3, const v4u &n4, const v4u &n13, const v4u &n14, const v4u &n16, v4u position) {
    const v4u p = vitof(position, 12);
    v4u acc = vmulz(n1, vsplat(p[0]));
    acc = vmaddz(acc, n2, vsplat(p[1]));
    acc = vmaddz(acc, n3, vsplat(p[2]));
    Seen s;
    s.clip = vmaddz(acc, n4, vsplat(kOne));
    s.q = fp::div(kOne, s.clip[3]).value;
    const v4u screen = with_xyz(s.clip, vmulz(s.clip, vfix(vsplat(s.q))));
    s.inside = static_cast<std::uint16_t>(signs_xyw(vsubz(n13, screen)) & kInside & signs_xyw(vsubz(screen, n14)));
    s.behind = (vsubz(s.clip, vsplat(n16[0]))[3] & 0x80000000u) != 0;
    return s;
}

bool fits16(std::uint32_t v) { return static_cast<std::int32_t>(v) >= -32768 && static_cast<std::int32_t>(v) <= 32767; }

// Writes the packet at TOP + 201; false when the batch cannot be one.
bool pack_batch(const Vu0Regs &r, std::uint8_t *mem, std::uint32_t top, std::uint32_t count, const v4u &tag) {
    if (count < 4 || 9 + 4 * count > 201) return false;
    const std::uint32_t at = top + 201u;
    const std::uint32_t uv_z = vfix(load(mem, top + 11))[2];
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::uint32_t base = top + 9 + 4 * k;
        const v4u pos = load(mem, base), colour = load(mem, base + 1), uv = load(mem, base + 2);
        if (!fits16(pos[0]) || !fits16(pos[1]) || !fits16(pos[2]) || !fits16(uv[0]) || !fits16(uv[1])) return false;
        if ((colour[0] | colour[1] | colour[2] | colour[3]) > 255u || vfix(uv)[2] != uv_z) return false;
        const v4u packed{(pos[0] & 0xFFFFu) | (pos[1] << 16), (pos[2] & 0xFFFFu) | (pos[3] << 16),
                         colour[0] | (colour[1] << 8) | (colour[2] << 16) | (colour[3] << 24), (uv[0] & 0xFFFFu) | (uv[1] << 16)};
        store(mem, at + 11 + k, packed);
    }
    const std::uint32_t data = 10 + count;
    store(mem, at, v4u{data | 0x8000u, 0u, 0x10000000u, 0xFu}); // NLOOP, EOP; FLG PACKED, NREG 1; REGS NOP
    store(mem, at + 1, v4u{kBatchMagic, kBatchVersion, count, uv_z});
    store(mem, at + 2, tag);
    const int consts[8] = {1, 2, 3, 4, 13, 14, 15, 16};
    for (int i = 0; i < 8; ++i) store(mem, at + 3 + i, vfix(vload(r.vf[consts[i]])));
    return true;
}

// The registers and memory the program leaves for the next batch, from the
// batch's last vertices (see dc7a for what each one is).
void finish_state(Vu0Regs &r, std::uint8_t *mem, std::uint32_t top, std::uint32_t count, const v4u &tag) {
    const v4u n1 = vfix(vload(r.vf[1])), n2 = vfix(vload(r.vf[2])), n3 = vfix(vload(r.vf[3])), n4 = vfix(vload(r.vf[4]));
    const v4u n13 = vfix(vload(r.vf[13])), n14 = vfix(vload(r.vf[14])), n15 = vfix(vload(r.vf[15])), n16 = vfix(vload(r.vf[16]));
    const v4u header = load(mem, top);
    const v4u f17 = vload(r.vf[17]);
    const std::uint32_t first = top + 9u;
    auto flag = [&](std::uint32_t k) { return static_cast<std::uint16_t>(load(mem, first + 4 * k)[3]); };

    // The last four vertices decide VI7..VI12 and the clipper marks of the
    // last two; VI10 (2 after an inside vertex, one less after each outside
    // one) needs the outside run before them. VI6 is only the marks of these
    // four: the clipper batches it would flag are not told apart here.
    constexpr std::uint32_t kTail = 4;
    const std::uint32_t from = count - kTail;
    auto inside_at = [&](std::uint32_t k) { return see(n1, n2, n3, n4, n13, n14, n16, load(mem, first + 4 * k)).inside; };
    Seen seen[kTail];
    for (std::uint32_t j = 0; j < kTail; ++j) seen[j] = see(n1, n2, n3, n4, n13, n14, n16, load(mem, first + 4 * (from + j)));
    std::uint16_t i10 = r.vi[10];
    for (std::uint32_t k = from; k-- > 0;) {
        if (inside_at(k) == kInside) {
            i10 = static_cast<std::uint16_t>(2u - (from - 1u - k));
            break;
        }
        if (k == 0) i10 = static_cast<std::uint16_t>(r.vi[10] - from);
    }
    std::uint16_t i7 = r.vi[7], i8 = r.vi[8];
    for (std::uint32_t k = from >= 2 ? from - 2 : 0; k < from; ++k) {
        const std::uint16_t in = inside_at(k);
        i8 = static_cast<std::uint16_t>(i7 & in);
        i7 = in;
    }
    std::uint16_t i9 = r.vi[9], i11 = r.vi[11], i12 = r.vi[12];
    std::uint32_t marks[kTail] = {};
    for (std::uint32_t j = 0; j < kTail; ++j) {
        const std::uint16_t f = flag(from + j);
        const std::uint16_t i1 = seen[j].inside;
        i10 = i1 == kInside ? std::uint16_t{2} : static_cast<std::uint16_t>(i10 - 1u);
        i11 = static_cast<std::uint16_t>(static_cast<std::uint16_t>(f + 32767u) | i10);
        i12 = static_cast<std::uint16_t>(f - 32561u);
        i9 = static_cast<std::uint16_t>(i8 & i1);
        i8 = static_cast<std::uint16_t>(i7 & i1);
        i7 = i1;
        if (i9 != kInside && static_cast<std::int16_t>(i11) >= 0) marks[j] = seen[j].behind ? 2u : 0u;
    }

    // The last two vertices as the program leaves them in place: clip
    // position, colour (alpha scaled), uv with the clipper mark in w.
    v4u kept[2][3];
    for (std::uint32_t j = 0; j < 2; ++j) {
        const std::uint32_t base = first + 4 * (count - 2 + j);
        v4u colour = vitof(load(mem, base + 1), 0);
        colour[3] = vmulz(colour, n16)[3];
        v4u uv = vitof(load(mem, base + 2), 12);
        uv[2] = vaddz(vfix(load(mem, base + 2)), v4u{0u, 0u, 0u, kOne})[2];
        uv[3] = marks[kTail - 2 + j];
        kept[j][0] = seen[kTail - 2 + j].clip;
        kept[j][1] = colour;
        kept[j][2] = uv;
        for (std::uint32_t i = 0; i < 3; ++i) store(mem, base + i, kept[j][i]);
    }
    const std::uint32_t itop = vu::itop();
    store(mem, itop + 1u, kept[0][0]);
    store(mem, itop + 2u, kept[0][1]);
    store(mem, itop + 3u, kept[0][2]);
    store(mem, itop + 5u, kept[1][0]);
    store(mem, itop + 6u, kept[1][1]);

    // The fog of the last vertex in VF18.w, as the loop leaves it.
    const Seen &last = seen[kTail - 1];
    v4u acc_w{0u, 0u, 0u, vmulz(v4u{0u, 0u, 0u, kOne}, vsplat(n15[2]))[3]};
    v4u f18 = tag;
    f18[3] = vmaddz(acc_w, last.clip, vsplat(n15[3]))[3];
    f18 = with_w(f18, vmax(f18, vsplat(r.vf[15].w[0])));
    f18 = with_w(f18, vmin(f18, vsplat(r.vf[15].w[1])));
    f18 = with_w(f18, vftoi(f18, 4));
    // ACC: the loop has already started on the qword after the batch.
    const v4u past = vitof(load(mem, first + 4 * count), 12);
    v4u acc = vmulz(n1, vsplat(past[0]));
    acc = vmaddz(acc, n2, vsplat(past[1]));
    acc = vmaddz(acc, n3, vsplat(past[2]));
    acc[3] = acc_w[3];

    v4u f8 = vload(r.vf[8]);
    f8 = v4u{header[0], f8[1], f17[2], header[3]};
    vstore(r.vf[8], f8);
    vstore(r.vf[18], f18);
    vstore(r.vf[20], kept[0][0]);
    vstore(r.vf[21], kept[0][1]);
    vstore(r.vf[22], kept[0][2]);
    vstore(r.vf[23], kept[1][0]);
    vstore(r.vf[24], kept[1][1]);
    vstore(r.vf[25], kept[1][2]);
    vstore(r.acc, acc);
    r.q = last.q;
    r.vi[1] = static_cast<std::uint16_t>(itop);
    r.vi[2] = static_cast<std::uint16_t>(first + 4 * count);
    r.vi[3] = 0;
    r.vi[4] = static_cast<std::uint16_t>(top + 202u + 4 * count);
    r.vi[5] = static_cast<std::uint16_t>(top + 201u);
    r.vi[6] = static_cast<std::uint16_t>(marks[0] | marks[1] | marks[2] | marks[3]);
    r.vi[7] = i7;
    r.vi[8] = i8;
    r.vi[9] = i9;
    r.vi[10] = i10;
    r.vi[11] = i11;
    r.vi[12] = i12;
    r.vi[13] = kInside;
    r.tpc = 0x50u;
}

// UN5_STAGE_REF: the packet at TOP + 201 turned back into the program's
// strip, with the vertex shader's float math. A vertex ends a triangle when
// its strip flag is clear and it and the two before it are inside the guard
// band; the others get ADC. The strip goes on from the batch before (its
// tag has no PRE), so the first two vertices look back at that batch's last
// two, whose guard band results the program keeps in VI7 and VI8.
void expand_reference(std::uint8_t *mem, std::uint32_t top, std::uint16_t i7, std::uint16_t i8) {
    const std::uint32_t at = top + 201u;
    const v4u info = load(mem, at + 1);
    const std::uint32_t count = info[2];
    const std::uint32_t uv_z_bits = info[3];
    float uv_z;
    std::memcpy(&uv_z, &uv_z_bits, 4);
    const v4u tag = load(mem, at + 2);
    float c[8][4];
    for (int i = 0; i < 8; ++i) std::memcpy(c[i], qword(mem, at + 3 + i), 16);
    const float(&g13)[4] = c[4], (&g14)[4] = c[5], (&fog)[4] = c[6], (&g16)[4] = c[7];
    v4u packed[48];
    for (std::uint32_t k = 0; k < count; ++k) packed[k] = load(mem, at + 11 + k);
    for (std::uint32_t k = 0; k < count; ++k) {
        const v4u v = packed[k];
        const float x = static_cast<float>(static_cast<std::int16_t>(v[0])) / 4096.0f;
        const float y = static_cast<float>(static_cast<std::int16_t>(v[0] >> 16)) / 4096.0f;
        const float z = static_cast<float>(static_cast<std::int16_t>(v[1])) / 4096.0f;
        float clip[4];
        for (int l = 0; l < 4; ++l) clip[l] = c[0][l] * x + c[1][l] * y + c[2][l] * z + c[3][l];
        const float q = 1.0f / clip[3];
        const float sx = clip[0] * q, sy = clip[1] * q, sz = clip[2] * q;
        const std::uint16_t in = sx > g13[0] && sx < g14[0] && sy > g13[1] && sy < g14[1] && clip[3] > g13[3] && clip[3] < g14[3] ? kInside : 0;
        const std::uint16_t i9 = static_cast<std::uint16_t>(i8 & in);
        i8 = static_cast<std::uint16_t>(i7 & in);
        i7 = in;
        const float u = static_cast<float>(static_cast<std::int16_t>(v[3])) / 4096.0f;
        const float t = static_cast<float>(static_cast<std::int16_t>(v[3] >> 16)) / 4096.0f;
        const float f = std::min(std::max(fog[2] + clip[3] * fog[3], fog[0]), fog[1]);
        const std::uint32_t alpha = static_cast<std::uint32_t>(static_cast<float>(v[2] >> 24) * g16[3]);
        const bool drawn = (v[1] >> 16) == 0 && i9 == kInside;
        const float st[4] = {u * q, t * q, uv_z * q, 0.0f};
        v4u stq;
        std::memcpy(&stq, st, 16);
        const std::uint32_t base = at + 1 + 4 * k;
        store(mem, base, v4u{0u, 0u, 0u, static_cast<std::uint32_t>(static_cast<std::int32_t>(f * 16.0f))});
        store(mem, base + 1, stq);
        store(mem, base + 2, v4u{v[2] & 0xFFu, (v[2] >> 8) & 0xFFu, (v[2] >> 16) & 0xFFu, alpha});
        store(mem, base + 3, v4u{static_cast<std::uint32_t>(static_cast<std::int32_t>(sx * 16.0f)),
                                 static_cast<std::uint32_t>(static_cast<std::int32_t>(sy * 16.0f)),
                                 static_cast<std::uint32_t>(static_cast<std::int32_t>(sz * 16.0f)), drawn ? 0u : 0x8000u});
    }
    store(mem, at, tag);
}

// Whether the program would run its clipper on this batch (dc7a's first loop).
bool needs_clipper(const Vu0Regs &r, std::uint8_t *mem, std::uint32_t top, std::uint32_t count) {
    const v4u n1 = vfix(vload(r.vf[1])), n2 = vfix(vload(r.vf[2])), n3 = vfix(vload(r.vf[3])), n4 = vfix(vload(r.vf[4]));
    const v4u n13 = vfix(vload(r.vf[13])), n14 = vfix(vload(r.vf[14])), n16 = vfix(vload(r.vf[16]));
    std::uint16_t i7 = r.vi[7], i8 = r.vi[8], i10 = r.vi[10];
    for (std::uint32_t k = 0; k < count; ++k) {
        const v4u in0 = load(mem, top + 9 + 4 * k);
        const Seen s = see(n1, n2, n3, n4, n13, n14, n16, in0);
        i10 = s.inside != kInside ? static_cast<std::uint16_t>(i10 - 1u) : std::uint16_t{2};
        const std::uint16_t i11 = static_cast<std::uint16_t>(static_cast<std::uint16_t>(static_cast<std::uint16_t>(in0[3]) + 32767u) | i10);
        const std::uint16_t i9 = static_cast<std::uint16_t>(i8 & s.inside);
        i8 = static_cast<std::uint16_t>(i7 & s.inside);
        i7 = s.inside;
        if (i9 != kInside && static_cast<std::int16_t>(i11) >= 0 && s.behind) return true;
    }
    return false;
}

void dc7a_batch(Vu0Regs &r, std::uint8_t *mem) {
    const std::uint32_t top = vu::top();
    const std::uint32_t count = load(mem, top)[0] & 0x7FFFu;
    const bool reference = !gpu_batches();
    if (reference && needs_clipper(r, mem, top, count)) {
        vu::native_fallback()(r, mem);
        return;
    }
    store(mem, r.vi[1] + 7u, vload(r.vf[25]));
    const v4u header = load(mem, top), f17 = vload(r.vf[17]);
    const v4u tag{header[0], f17[1], f17[2], header[3]};
    if (!pack_batch(r, mem, top, count, tag)) {
        vu::native_fallback()(r, mem);
        return;
    }
    const std::uint16_t i7_in = r.vi[7], i8_in = r.vi[8];
    finish_state(r, mem, top, count, tag);
    if (reference) expand_reference(mem, top, i7_in, i8_in);
    vu::kick(1, top + 201u);
}

void dc7a_entry(Vu0Regs &r, std::uint8_t *mem) {
    if (gpu_batches() || reference_batches()) dc7a_batch(r, mem);
    else dc7a(r, mem);
}

struct Register {
    Register() { vu::register_native(0xDC7A318002C3DD86ull, dc7a_entry); }
} g_register;

} // namespace
