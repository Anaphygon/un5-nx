// SPDX-License-Identifier: GPL-3.0+
// Decrypts UN5's DATA/DATA.CVM with the game's own code, for studying its
// assets. The CVM is a CRI ROFS volume: a header, then an ISO 9660 image whose
// system area is encrypted, sector by sector. The game decrypts a sector right after
// reading it, in its function at 0x00123788 (buffer, bytes, sector size,
// sector number, key context, 8), for the volume's system area only: the
// descriptors and directories before the first file. The files themselves are
// stored as they are (gzip). This links ps2-web-recomp's runtime and your
// translation of the game, loads a RAM dump taken after the volume is mounted
// (for the key context at 0x0062EFAC) and calls that function once per sector.
//
//   build: see tools/cvm_decrypt.sh
//   cvm_decrypt <RAM dump (PS2_DUMP)> <DATA.CVM> <out.iso> <first file sector>
//
// The output is your own disc's data: keep it local.
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

void register_generated_code();

namespace {

constexpr std::uint32_t kDecrypt = 0x00123788u;
constexpr std::uint32_t kKeyContext = 0x0062EFACu;
constexpr std::uint32_t kGp = 0x00617EF0u;        // the game's $gp
constexpr std::uint32_t kBuffer = 0x01E00000u;    // free RAM for one sector
constexpr std::uint32_t kStack = 0x01F00000u;
constexpr std::uint32_t kSector = 2048;
constexpr std::uint64_t kImageStart = 0x1800;      // CVMH and ZONE chunks before the ISO

} // namespace

int main(int argc, char **argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: cvm_decrypt <RAM dump> <DATA.CVM> <out.iso> <first file sector>\n");
        return 1;
    }
    register_generated_code();
    std::FILE *dump = std::fopen(argv[1], "rb");
    if (!dump || std::fread(ps2::g_ram, 1, ps2::kRamSize, dump) != ps2::kRamSize) {
        std::fprintf(stderr, "cannot read the RAM dump %s\n", argv[1]);
        return 1;
    }
    std::fclose(dump);
    std::FILE *in = std::fopen(argv[2], "rb");
    std::FILE *out = std::fopen(argv[3], "wb");
    if (!in || !out) {
        std::fprintf(stderr, "cannot open %s or %s\n", argv[2], argv[3]);
        return 1;
    }
    std::fseek(in, 0, SEEK_END);
    const std::uint64_t size = static_cast<std::uint64_t>(std::ftell(in));
    const std::uint64_t sectors = (size - kImageStart) / kSector;
    const std::uint64_t system_area = std::strtoull(argv[4], nullptr, 10);
    std::fseek(in, static_cast<long>(kImageStart), SEEK_SET);
    std::vector<std::uint8_t> sector(kSector);
    for (std::uint64_t n = 0; n < sectors; ++n) {
        if (std::fread(sector.data(), 1, kSector, in) != kSector) break;
        if (n >= system_area) {
            std::fwrite(sector.data(), 1, kSector, out);
            continue;
        }
        std::memcpy(ps2::g_ram + kBuffer, sector.data(), kSector);
        ps2::Cpu &c = ps2::g_cpu;
        std::memset(&c, 0, sizeof c);
        c.r[4].d[0] = kBuffer;
        c.r[5].d[0] = kSector;
        c.r[6].d[0] = kSector;
        c.r[7].d[0] = n;
        c.r[8].d[0] = kKeyContext;
        c.r[9].d[0] = 8;
        c.r[28].d[0] = kGp;
        c.r[29].d[0] = kStack;
        c.r[31].d[0] = ps2::kIdle;
        ps2::call(c, kDecrypt, 0);
        if ((c.pc & ~1u) != ps2::kIdle) {
            std::fprintf(stderr, "sector %llu: the decrypt function stopped at 0x%08X\n", static_cast<unsigned long long>(n), c.pc);
            return 1;
        }
        std::fwrite(ps2::g_ram + kBuffer, 1, kSector, out);
        if (n % 20000 == 0) std::fprintf(stderr, "%llu / %llu sectors\n", static_cast<unsigned long long>(n), static_cast<unsigned long long>(sectors));
    }
    std::fclose(out);
    std::fclose(in);
    std::fprintf(stderr, "done: %llu sectors\n", static_cast<unsigned long long>(sectors));
    return 0;
}
