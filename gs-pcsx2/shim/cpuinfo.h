// Stand-in for cpuinfo: GS only asks it about x86 vector extensions, which an
// ARM CPU never has.
#pragma once
#include <cstdint>
enum cpuinfo_vendor { cpuinfo_vendor_unknown, cpuinfo_vendor_intel };
enum cpuinfo_uarch { cpuinfo_uarch_unknown, cpuinfo_uarch_haswell };
struct cpuinfo_core { cpuinfo_vendor vendor; };
struct cpuinfo_uarch_info { cpuinfo_uarch uarch; };
inline bool cpuinfo_initialize() { return true; }
inline bool cpuinfo_has_x86_avx() { return false; }
inline bool cpuinfo_has_x86_avx2() { return false; }
inline bool cpuinfo_has_x86_avx512f() { return false; }
inline bool cpuinfo_has_x86_fma3() { return false; }
inline bool cpuinfo_has_x86_bmi() { return false; }
inline bool cpuinfo_has_x86_bmi2() { return false; }
inline std::uint32_t cpuinfo_get_cores_count() { return 0; }
inline const cpuinfo_core *cpuinfo_get_core(std::uint32_t) { static const cpuinfo_core core{}; return &core; }
inline std::uint32_t cpuinfo_get_uarchs_count() { return 0; }
inline const cpuinfo_uarch_info *cpuinfo_get_uarch(std::uint32_t) { static const cpuinfo_uarch_info info{}; return &info; }
