// SPDX-License-Identifier: GPL-3.0+
// The clocks a speed log was measured at, read from clkrst: Horizon OC can
// set any of them. Apart from PCSX2's headers for libnx's u128.
#include <switch.h>

#include <cstring>
#include <initializer_list>

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

// The clock service: Horizon OC's hoc:clk, or else sys-clk's sys:clk (also
// what HATS-style packs and sys-clk-oc forks run; Tesla and Ultrahand are only
// overlay loaders on top). Both speak the sys-clk protocol (commands from
// github.com/retronx-team/sys-clk and github.com/Horizon-OC/Horizon-OC): the
// override is the one the overlay sets. Temperatures and power are read
// with Horizon OC's context layout only.
namespace
{
	Service g_hoc;
	bool g_hoc_open = false, g_hoc_tried = false, g_is_hoc = false;
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
	if (g_hoc_open || g_hoc_tried)
		return g_hoc_open;
	g_hoc_tried = true;
	for (const char* name : {"hoc:clk", "sys:clk"})
	{
		if (R_FAILED(smGetService(&g_hoc, name)))
			continue;
		u32 version = 0;
		if (R_SUCCEEDED(serviceDispatchOut(&g_hoc, kGetApiVersion, version)))
		{
			g_hoc_open = true;
			g_is_hoc = name[0] == 'h';
			return true;
		}
		serviceClose(&g_hoc);
	}
	return false;
}

// "Horizon OC", "sys-clk" or nullptr when neither runs.
const char* un5_clock_service_name()
{
	return un5_hoc_open() ? (g_is_hoc ? "Horizon OC" : "sys-clk") : nullptr;
}

// The supported frequency nearest to mhz, in Hz. The list is asked for in
// full: 64 for Horizon OC (with 32, 1963 MHz was cut off and the nearest left
// was 1836), 32 for sys-clk, whose service wants maxCount to match the buffer
// and goes no further. Without a list the request goes as it is and the
// service picks.
u32 un5_hoc_nearest_hz(u32 module, u32 mhz)
{
	if (!g_hoc_open)
		return 0;
	u32 list[64] = {}, count = 0;
	bool listed = false;
	for (const u32 max : {64u, 32u})
	{
		const struct { u32 module, max_count; } args = {module, max};
		if (R_SUCCEEDED(serviceDispatchInOut(&g_hoc, kGetFreqList, args, count,
				.buffer_attrs = {SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out},
				.buffers = {{list, max * sizeof(u32)}})))
		{
			listed = count > 0;
			count = count < max ? count : max;
			break;
		}
	}
	if (!listed)
		return mhz * 1000000;
	u32 best = 0;
	for (u32 i = 0; i < count; ++i)
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
	if (!g_hoc_open || !g_is_hoc || R_FAILED(serviceDispatch(&g_hoc, kGetCurrentContext,
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

// The in-game menu's view of the controller and the console's font, here
// because this file may include libnx (ingame_menu.cpp includes PCSX2).
namespace
{
	PadState g_menu_pad;
	bool g_menu_pad_ready = false;
} // namespace

// Bits: 1 A, 2 B, 4 up, 8 down, 16 left, 32 right, 64 L3, 128 R3.
void un5_pad_read(u32& held, u32& down)
{
	if (!g_menu_pad_ready)
	{
		padInitializeAny(&g_menu_pad);
		g_menu_pad_ready = true;
	}
	padUpdate(&g_menu_pad);
	const auto map = [](u64 b) {
		return ((b & HidNpadButton_A) ? 1u : 0u) | ((b & HidNpadButton_B) ? 2u : 0u) | ((b & HidNpadButton_AnyUp) ? 4u : 0u) |
			   ((b & HidNpadButton_AnyDown) ? 8u : 0u) | ((b & HidNpadButton_AnyLeft) ? 16u : 0u) |
			   ((b & HidNpadButton_AnyRight) ? 32u : 0u) | ((b & HidNpadButton_StickL) ? 64u : 0u) | ((b & HidNpadButton_StickR) ? 128u : 0u);
	};
	held = map(padGetButtons(&g_menu_pad));
	down = map(padGetButtonsDown(&g_menu_pad));
}

// The console's standard shared font, for ImGui (not copied, owned by pl).
bool un5_shared_font(const void** data, std::size_t* size)
{
	static bool ready = false;
	static PlFontData font;
	if (!ready)
	{
		if (R_FAILED(plInitialize(PlServiceType_User)) || R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard)))
			return false;
		ready = true;
	}
	*data = font.address;
	*size = font.size;
	return true;
}
