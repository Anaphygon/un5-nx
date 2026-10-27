// SPDX-License-Identifier: GPL-3.0+
// common/HostSys's tick counter for the Switch: the ARM system counter.
// Apart from PCSX2's headers for libnx's u128.
#include <switch.h>

u64 GetTickFrequency()
{
	return armGetSystemTickFreq();
}

u64 GetCPUTicks()
{
	return armGetSystemTick();
}
