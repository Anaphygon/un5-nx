// SPDX-License-Identifier: GPL-3.0+
// What the Switch build gets from libnx and its own files, for the Mac shader
// harvest (UN5_HOST): the tick counter, the window the deko3d device presents
// to (none here), and the console's shader compiler, which here builds
// nothing, so the device keeps every new shader's source for the Mac's uam
// (tools/dk_shaders.py).
#include <mach/mach_time.h>

#include <cstdint>

std::uint64_t GetTickFrequency()
{
	mach_timebase_info_data_t base;
	mach_timebase_info(&base);
	return 1'000'000'000ull * base.denom / base.numer;
}

std::uint64_t GetCPUTicks()
{
	return mach_absolute_time();
}

struct NWindow;
extern "C" NWindow* nwindowGetDefault(void)
{
	static char window;
	return reinterpret_cast<NWindow*>(&window);
}
extern "C" std::uint32_t nwindowSetSwapInterval(NWindow*, std::uint32_t)
{
	return 0;
}

bool un5_uam_compile_big_stack(int, const char*, const char*)
{
	return false;
}
