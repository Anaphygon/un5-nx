// SPDX-License-Identifier: GPL-3.0+
// The clocks a speed log was measured at, read from clkrst: Horizon OC can
// set any of them. Apart from PCSX2's headers for libnx's u128.
#include <switch.h>

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
