// SPDX-License-Identifier: GPL-3.0+
// The clocks a speed log was measured at, read from clkrst: Horizon OC can
// set any of them. Apart from PCSX2's headers for libnx's u128.
#include <switch.h>

#include <cstring>

// MHz of the CPU (0), GPU (1) and memory (2); 0 when unreadable.
unsigned un5_clock_mhz(int which)
{
	static const PcvModuleId kModules[] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
	if (which < 0 || which > 2 || R_FAILED(clkrstInitialize()))
		return 0;
	ClkrstSession session;
	u32 hz = 0;
	if (R_SUCCEEDED(clkrstOpenSession(&session, kModules[which], 3)))
	{
		clkrstGetClockRate(&session, &hz);
		clkrstCloseSession(&session);
	}
	clkrstExit();
	return hz / 1000000;
}

// The CPU clock as the CPU runs it, in MHz: eight dependent adds per loop
// turn take eight cycles on a Cortex-A57, timed against the system counter.
// Best of three, so a preempted try does not count. About 5 ms.
unsigned un5_measured_cpu_mhz()
{
	constexpr u64 kTurns = 1000000;
	u64 best = ~0ull;
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		u64 x = 0;
		const u64 start = armGetSystemTick();
		for (u64 i = 0; i < kTurns; ++i)
			asm volatile("add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
						 "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1"
						 : "+r"(x));
		const u64 ticks = armGetSystemTick() - start;
		if (ticks < best)
			best = ticks;
	}
	const double seconds = static_cast<double>(best) / static_cast<double>(armGetSystemTickFreq());
	return static_cast<unsigned>(static_cast<double>(kTurns * 8) / seconds / 1e6);
}

// Horizon OC's clock service (hoc:clk, the sys-clk protocol: commands and
// layouts from github.com/Horizon-OC/Horizon-OC, Source/hoc-clk/common). A
// benchmark run can set the clocks itself through its override, the same
// override the overlay sets, and read temperatures and power.
namespace
{
	Service g_hoc;
	bool g_hoc_open = false;
	constexpr u32 kGetApiVersion = 0, kGetCurrentContext = 2, kSetOverride = 8, kGetFreqList = 11;
	constexpr u32 kModules = 3; // CPU, GPU, memory

	// The fixed-layout head of HocClkContext ("stable", never changes).
	struct StableContext
	{
		u32 freqs[5], real_freqs[5], override_freqs[5];
		s32 temps[11]; // millidegrees: SoC, PCB, skin, battery, PMIC, CPU, GPU, memory, ...
		s32 power[2];  // mW: now, average
	};
} // namespace

bool un5_hoc_open()
{
	if (g_hoc_open)
		return true;
	if (R_FAILED(smGetService(&g_hoc, "hoc:clk")))
		return false;
	u32 version = 0;
	if (R_FAILED(serviceDispatchOut(&g_hoc, kGetApiVersion, version)))
	{
		serviceClose(&g_hoc);
		return false;
	}
	g_hoc_open = true;
	return true;
}

// The supported frequency nearest to mhz, in Hz (0 when none is known). The
// list is asked for in full (64): with 32 entries 1963 MHz was cut off and the
// nearest left was 1836.
u32 un5_hoc_nearest_hz(u32 module, u32 mhz)
{
	constexpr u32 kMax = 64; // HOCCLK_FREQ_LIST_MAX; the service wants maxCount to match the buffer
	u32 list[kMax] = {}, count = 0;
	const struct { u32 module, max_count; } args = {module, kMax};
	if (!g_hoc_open || R_FAILED(serviceDispatchInOut(&g_hoc, kGetFreqList, args, count,
						   .buffer_attrs = {SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out},
						   .buffers = {{list, sizeof(list)}})))
		return 0;
	u32 best = 0;
	for (u32 i = 0; i < count && i < kMax; ++i)
	{
		const s64 d = static_cast<s64>(list[i]) - static_cast<s64>(mhz) * 1000000;
		const s64 b = static_cast<s64>(best) - static_cast<s64>(mhz) * 1000000;
		if (!best || (d < 0 ? -d : d) < (b < 0 ? -b : b))
			best = list[i];
	}
	return best;
}

// Overrides CPU, GPU and memory (hz 0 lifts an override).
bool un5_hoc_override(const u32 hz[3])
{
	bool ok = g_hoc_open;
	for (u32 m = 0; ok && m < kModules; ++m)
	{
		const struct { u32 module, hz; } args = {m, hz[m]};
		ok = R_SUCCEEDED(serviceDispatchIn(&g_hoc, kSetOverride, args));
	}
	return ok;
}

// SoC, CPU and GPU temperature in tenths of a degree, power draw in mW.
bool un5_hoc_status(int out[5])
{
	alignas(8) static u8 buffer[8192];
	if (!g_hoc_open || R_FAILED(serviceDispatch(&g_hoc, kGetCurrentContext,
						   .buffer_attrs = {SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out},
						   .buffers = {{buffer, sizeof(buffer)}})))
		return false;
	StableContext c;
	std::memcpy(&c, buffer, sizeof c);
	out[0] = c.temps[0] / 100;
	out[1] = c.temps[5] / 100;
	out[2] = c.temps[6] / 100;
	out[3] = c.power[0];
	out[4] = c.power[1];
	return true;
}
