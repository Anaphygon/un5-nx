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
