// SPDX-License-Identifier: GPL-3.0+
// un5-nx's front end to PCSX2's GS, in place of PCSX2's GS.cpp (which belongs
// to its VM: settings, hotkeys, captures). It opens the OpenGL device and the
// hardware renderer and forwards what the runtime's GIF unit sees.
// Needs proper testing on hardware: the flow follows GS.cpp, trimmed.
#include "un5gs.h"

#include "GS/GS.h"
#include "GS/GSPerfMon.h"
#include "GS/GSState.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/OpenGL/GSDeviceOGL.h"
#include "common/Console.h"
#include "common/Timer.h"
#include "fmt/format.h"

#include <algorithm>
#include <cstring>
#include <vector>
#include <array>
#include <iterator>
#include <string>
#include <cstdio>
#include <cstdlib>

Pcsx2Config::GSOptions GSConfig;

// Raw storage in emu_config_storage.cpp; see there.
static_assert(sizeof(Pcsx2Config) <= (1 << 16));

// From patches/0003: time spent in GLShaderCache::GetProgram.
extern double g_un5_program_ms;
// From the runner (ps2/system.hpp): VU1 program runs left to the interpreter.
namespace ps2 { std::uint64_t vu1_interpreted_calls(); double vu_thread_idle_ms(); }
// From patches/0004: why frames split into draws, and where a draw's time goes.
extern u64 g_un5_flush_reason[16];
extern u64 g_un5_split[4];
extern u64 g_un5_indices;
extern double g_un5_draw_ms, g_un5_render_ms, g_un5_swap_ms;
extern u64 g_un5_gpu_behind, g_un5_presents;
#ifdef __SWITCH__
// From switch_clocks.cpp.
unsigned un5_clock_mhz(int which);
unsigned un5_measured_cpu_mhz();
void un5_menu_poll();
void un5_osd_begin();
bool un5_hoc_open();
u32 un5_hoc_nearest_hz(u32 module, u32 mhz);
bool un5_hoc_override(const u32 hz[3]);
bool un5_hoc_status(int out[5]);
// From ps2-web-recomp's host/switch_threads.cpp: the core for threads
// created from now on (-1: libnx's default).
extern "C" void ps2_switch_new_thread_core(int core);
// ... and moves the last thread placed that way (glthread's worker).
extern "C" bool ps2_switch_move_placed_thread(int core);
int s_glthread_core = 1;
#endif

namespace {
alignas(16) u8 s_privileged[0x2000];

// GS settings from the environment (args.txt on the Switch), to trade
// accuracy for speed without a rebuild:
//   UN5GS_PRELOAD   texture preloading 0 off, 1 partial, 2 full (PCSX2 default)
//   UN5GS_BLEND     blending accuracy 0 minimum .. 5 maximum (default 0: measured
//                   on a Switch, about 1 ms off the GS thread and +1-2 fps in battle,
//                   nothing wrong on screen; PCSX2's own default is 1, basic)
//   UN5GS_MIPMAP    hardware mipmapping 0 or 1 (default 1)
//   UN5GS_DITHER    dithering 0 off .. 2 (default 2)
//   UN5GS_DOWNLOAD  GS-to-EE readbacks, PCSX2's GSHardwareDownloadMode: 0 enabled
//                   (default), 1 enabled and full, 2 no readbacks, 3 unsynchronized, 4 disabled
//   UN5GS_UPSCALE   internal resolution, 1 native .. 3 (default 2: sharper, and the
//                   sharp filter costs ~6 ms a frame at native, next to nothing at 2x)
//   UN5GS_FILTER    screen filter: 0 off (sharp pixels), 1 smooth (PCSX2's
//                   default, soft on an OLED), 2 sharp bilinear (default 2)
//   UN5GS_VSYNC     wait for the display's vertical blank when presenting (default 0:
//                   the runner already paces frames, and the Switch shows 60 Hz to a
//                   50 Hz game, so waiting stalls the render thread)
void apply_environment()
{
	const auto number = [](const char* name, int fallback) {
		const char* value = std::getenv(name);
		return value ? std::atoi(value) : fallback;
	};
	GSConfig.TexturePreloading = static_cast<TexturePreloadingLevel>(
		std::clamp(number("UN5GS_PRELOAD", static_cast<int>(GSConfig.TexturePreloading)), 0, 2));
	GSConfig.AccurateBlendingUnit = static_cast<AccBlendLevel>(std::clamp(number("UN5GS_BLEND", 0), 0, 5));
	GSConfig.HWMipmap = number("UN5GS_MIPMAP", GSConfig.HWMipmap ? 1 : 0) != 0;
	GSConfig.Dithering = static_cast<u8>(std::clamp(number("UN5GS_DITHER", GSConfig.Dithering), 0, 2));
	GSConfig.HWDownloadMode = static_cast<GSHardwareDownloadMode>(
		std::clamp(number("UN5GS_DOWNLOAD", static_cast<int>(GSConfig.HWDownloadMode)), 0, 4));
	GSConfig.UpscaleMultiplier = static_cast<float>(std::clamp(number("UN5GS_UPSCALE", 2), 1, 3));
	GSConfig.LinearPresent = static_cast<GSPostBilinearMode>(std::clamp(number("UN5GS_FILTER", 2), 0, 2));
	Console.WriteLn("un5gs: texture preloading %d, blending %d, mipmap %d, dithering %d, download %d, upscale %.0fx, filter %d",
		static_cast<int>(GSConfig.TexturePreloading), static_cast<int>(GSConfig.AccurateBlendingUnit), GSConfig.HWMipmap ? 1 : 0,
		GSConfig.Dithering, static_cast<int>(GSConfig.HWDownloadMode), GSConfig.UpscaleMultiplier, static_cast<int>(GSConfig.LinearPresent));
}

// Shader compiles, counted from PCSX2's log into the speed line.
u32 s_shader_compiles = 0;

// Time the render thread spends inside the GS, to tell a GS-bound frame from
// one that waits on emulation.
Common::Timer::Value s_gs_ticks = 0;
// The part of it spent in vsync(): the last draw of the frame and the present.
Common::Timer::Value s_vsync_ticks = 0;

struct GsTime
{
	Common::Timer::Value start = Common::Timer::GetCurrentValue();
	~GsTime() { s_gs_ticks += Common::Timer::GetCurrentValue() - start; }
};


#ifdef __SWITCH__
bool s_clock_override = false;

void clock_cycle_window()
{
	const char* spec = std::getenv("UN5_CLOCK_CYCLE");
	if (!spec)
		return;
	static std::vector<std::array<u32, 3>> s_sets;
	static int s_window = -1, s_rounds = 2;
	static bool s_done = false;
	if (s_window < 0)
	{
		if (!un5_hoc_open())
		{
			Console.WriteLn("[hoc] Horizon OC's hoc:clk is not running; clocks left alone");
			s_done = true;
		}
		for (const char* p = spec; *p && !s_done;)
		{
			unsigned c = 0, g = 0, m = 0;
			if (std::sscanf(p, "%u/%u/%u", &c, &g, &m) == 3)
				s_sets.push_back({c, g, m});
			const char* comma = std::strchr(p, ',');
			p = comma ? comma + 1 : p + std::strlen(p);
		}
		if (const char* r = std::getenv("UN5_CLOCK_ROUNDS"))
			s_rounds = std::max(1, std::atoi(r));
		s_window = 0;
	}
	int st[5] = {};
	if (s_clock_override && un5_hoc_status(st))
		Console.WriteLn("[hoc] SoC %d.%d C, CPU %d.%d C, GPU %d.%d C, power %d mW (avg %d)", st[0] / 10, st[0] % 10,
			st[1] / 10, st[1] % 10, st[2] / 10, st[2] % 10, st[3], st[4]);
	if (s_done || s_sets.empty())
		return;
	const int total = static_cast<int>(s_sets.size()) * 2 * s_rounds;
	if (s_window >= total)
	{
		const u32 lift[3] = {0, 0, 0};
		un5_hoc_override(lift);
		s_clock_override = false;
		s_done = true;
		Console.WriteLn("[hoc] cycle done; override lifted");
		return;
	}
	if (s_window % 2 == 0)
	{
		const auto& set = s_sets[(s_window / 2) % s_sets.size()];
		const u32 hz[3] = {un5_hoc_nearest_hz(0, set[0]), un5_hoc_nearest_hz(1, set[1]), un5_hoc_nearest_hz(2, set[2])};
		s_clock_override = un5_hoc_override(hz);
		Console.WriteLn("[hoc] next two windows: %u/%u/%u MHz (asked %u/%u/%u)%s", hz[0] / 1000000, hz[1] / 1000000,
			hz[2] / 1000000, set[0], set[1], set[2], s_clock_override ? "" : " - override failed");
	}
	++s_window;
}
#else
void clock_cycle_window() {}
#endif

// Why the GS split the last 5 s into draws, per frame: whether each draw had
// the same state as the one before it (so could have been merged), only a
// new texture, a new render target, or other state; why PCSX2 flushed; and
// the GS thread's time split between Draw() setup and GL submission.
void log_draw_split(u32 fields, double gs_ms)
{
	static const char* const kReasons[16] = {"unknown", "reset", "state change", "clut change", "gs transfer",
		"dirty texture", "dirty frame", "dirty z", "local move", "download", "save state", "load state",
		"autoflush", "vsync", "reopen", "vertex count"};
	const u64 draws = g_un5_split[0] + g_un5_split[1] + g_un5_split[2] + g_un5_split[3];
	const double per = fields ? 1.0 / fields : 0.0;
	const auto pct = [draws](u64 n) { return draws ? 100.0 * static_cast<double>(n) / static_cast<double>(draws) : 0.0; };
	std::string reasons;
	for (int i = 0; i < 16; i++)
		if (g_un5_flush_reason[i])
			reasons += fmt::format(" {} {:.0f}%,", kReasons[i], pct(g_un5_flush_reason[i]));
	if (!reasons.empty())
		reasons.pop_back();
	Console.WriteLn("[draws] %.0f per frame, %.1f indices each; vs the previous draw: same state %.0f%%, texture only %.0f%%, "
					"render target %.0f%%, other state %.0f%%; flushed for:%s; ms per frame: GS %.1f, Draw() %.1f, RenderHW %.1f, "
					"vsync() %.1f, SwapBuffers %.1f; GPU still busy at %.0f%% of presents",
		static_cast<double>(draws) * per, draws ? static_cast<double>(g_un5_indices) / static_cast<double>(draws) : 0.0,
		pct(g_un5_split[0]), pct(g_un5_split[1]), pct(g_un5_split[2]), pct(g_un5_split[3]), reasons.c_str(),
		gs_ms * per, g_un5_draw_ms * per, g_un5_render_ms * per,
		Common::Timer::ConvertValueToMilliseconds(s_vsync_ticks) * per, g_un5_swap_ms * per,
		g_un5_presents ? 100.0 * static_cast<double>(g_un5_gpu_behind) / static_cast<double>(g_un5_presents) : 0.0);
	s_vsync_ticks = 0;
	g_un5_swap_ms = 0.0;
	g_un5_gpu_behind = g_un5_presents = 0;
#ifdef __SWITCH__
	Console.WriteLn("[clocks] cpu %u (measured %u) gpu %u mem %u MHz; glthread worker on core %d", un5_clock_mhz(0),
		un5_measured_cpu_mhz(), un5_clock_mhz(1), un5_clock_mhz(2), s_glthread_core);
	// UN5_CLOCK_CYCLE="1224/768/1600,1785/921/1600,...": the run sets CPU/GPU/
	// memory itself through Horizon OC's override, each set for two 5 s
	// windows (the first lets it settle), UN5_CLOCK_ROUNDS times over (default
	// 2), then lifts the override. Temperatures and power go in the log.
	clock_cycle_window();
	// UN5GS_CYCLE=1: a different GS setting every 5 s, to price each within
	// one run: PCSX2's defaults, minimum blending accuracy, no dithering, no
	// texture preloading. (Mipmapping stays as is: switching it mid-run
	// would leave the texture cache inconsistent.)
	if (const char* cycle = std::getenv("UN5GS_CYCLE"); cycle && std::atoi(cycle))
	{
		static const Pcsx2Config::GSOptions s_defaults = GSConfig;
		static int s_step = 0;
		static const char* const kNames[] = {"defaults", "blending 0", "dithering 0", "preloading 0"};
		Console.WriteLn("[gs-cycle] this window: %s", kNames[s_step]);
		s_step = (s_step + 1) % 4;
		GSConfig.AccurateBlendingUnit = s_defaults.AccurateBlendingUnit;
		GSConfig.Dithering = s_defaults.Dithering;
		GSConfig.TexturePreloading = s_defaults.TexturePreloading;
		if (s_step == 1)
			GSConfig.AccurateBlendingUnit = AccBlendLevel::Minimum;
		else if (s_step == 2)
			GSConfig.Dithering = 0;
		else if (s_step == 3)
			GSConfig.TexturePreloading = TexturePreloadingLevel::Off;
	}
	// UN5_GLTHREAD_CYCLE=1: move the worker to the next core every 5 s, to
	// compare cores within one run.
	if (const char* cycle = std::getenv("UN5_GLTHREAD_CYCLE"); cycle && std::atoi(cycle))
	{
		const int next = (s_glthread_core + 1) % 3;
		if (ps2_switch_move_placed_thread(next))
			s_glthread_core = next;
	}
#endif
	static std::uint64_t s_interpreted = 0;
	const std::uint64_t interpreted = ps2::vu1_interpreted_calls();
	// The VU thread's share of the window, to tell a VU-bound drop (big jutsu)
	// from a GS- or GPU-bound one (the [draws] line has the GS and the GPU).
	static double s_vu_idle = 0;
	static Common::Timer::Value s_vu_since = Common::Timer::GetCurrentValue();
	const double vu_idle = ps2::vu_thread_idle_ms();
	const double window_ms = Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - s_vu_since);
	const double vu_busy = window_ms > 0 ? std::clamp(100.0 * (1.0 - (vu_idle - s_vu_idle) / window_ms), 0.0, 100.0) : 0.0;
	s_vu_idle = vu_idle;
	s_vu_since = Common::Timer::GetCurrentValue();
	Console.WriteLn("[vu1] VU thread busy %.0f%%; %.1f interpreted program runs per frame", vu_busy,
		static_cast<double>(interpreted - s_interpreted) * per);
	s_interpreted = interpreted;
	std::fill(std::begin(g_un5_flush_reason), std::end(g_un5_flush_reason), 0);
	std::fill(std::begin(g_un5_split), std::end(g_un5_split), 0);
	g_un5_indices = 0;
	g_un5_draw_ms = g_un5_render_ms = 0.0;
}

// Fields per second and the GS's share of the time, every 5 s: the Switch
// has no title bar to show them in.
void log_speed()
{
	static Common::Timer::Value s_start = 0;
	static u32 s_fields = 0;
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	if (!s_start)
		s_start = now;
	++s_fields;
	const double seconds = Common::Timer::ConvertValueToSeconds(now - s_start);
	if (seconds < 5.0)
		return;
	// GPU time from the device's timer queries: how much of it the GPU itself spent.
	const float gpu_ms = g_gs_device ? g_gs_device->GetAndResetAccumulatedGPUTime() : 0.0f;
	// Per frame, from PCSX2's own counters (averaged over its last 32 frames).
	Console.WriteLn("[speed] %.1f fps, GS busy %.0f%%, GPU busy %.0f%%, new shaders %u (%.0f ms getting programs), per frame: %.0f draws %.0f draw calls "
					"%.0f texture uploads %.0f texture copies %.0f readbacks %.0f barriers",
		static_cast<double>(s_fields) / seconds, Common::Timer::ConvertValueToSeconds(s_gs_ticks) / seconds * 100.0, gpu_ms / 10.0 / seconds,
		s_shader_compiles, g_un5_program_ms,
		g_perfmon.GetCounter(GSPerfMon::Draw), g_perfmon.GetCounter(GSPerfMon::DrawCalls), g_perfmon.GetCounter(GSPerfMon::TextureUploads),
		g_perfmon.GetCounter(GSPerfMon::TextureCopies), g_perfmon.GetCounter(GSPerfMon::Readbacks), g_perfmon.GetCounter(GSPerfMon::Barriers));
	log_draw_split(s_fields, Common::Timer::ConvertValueToSeconds(s_gs_ticks) * 1000.0);
	s_shader_compiles = 0;
	g_un5_program_ms = 0.0;
	s_start = now;
	s_fields = 0;
	s_gs_ticks = 0;
}

// PCSX2 logs through Console; the runner's log is stderr.
void log_to_stderr(LOGLEVEL, ConsoleColors, std::string_view message)
{
	if (message.find("Compiling new") != std::string_view::npos)
	{
		++s_shader_compiles;
		return;
	}
	std::fwrite(message.data(), 1, message.size(), stderr);
	if (message.empty() || message.back() != '\n')
		std::fputc('\n', stderr);
}
}

namespace un5gs {

// Mesa reads this when the GL context is made; args.txt can override it.
// glthread moves the driver's work per draw call to a thread of its own (a
// battle sends up to 18 000 draw calls a frame, and the GS thread spent its
// time in the driver). Mesa on Horizon has no disk cache; compiled programs
// are kept by PCSX2's own cache (cache/gl_programs.*), which needs the Mesa
// built from third_party_src/switch-mesa to report a program binary format.
void configure_mesa()
{
	setenv("mesa_glthread", "true", 0);
	Console.WriteLn("un5gs: mesa_glthread=%s", std::getenv("mesa_glthread"));
}

bool open()
{
	Log::SetHostOutputLevel(LOGLEVEL_DEV, log_to_stderr);
	configure_mesa();
	GSConfig.Renderer = GSRendererType::OGL;
	apply_environment();
	EmuConfig.CurrentAspectRatio = AspectRatioType::R4_3;
	EmuConfig.CurrentCustomAspectRatio = 0.0f;
	g_gs_device = std::make_unique<GSDeviceOGL>();
	const char* vsync = std::getenv("UN5GS_VSYNC");
#ifdef __SWITCH__
	// glthread's worker starts with the context, on core 1 with the emulation
	// thread, which waits often: measured 2026-10-29, cores 0 and 1 gave ~38
	// fps in battle, core 2 (the busy VU thread's) ~32. UN5_GLTHREAD_CORE
	// picks another (0-2).
	const char* gl_core = std::getenv("UN5_GLTHREAD_CORE");
	s_glthread_core = gl_core ? std::clamp(std::atoi(gl_core), 0, 2) : 1;
	ps2_switch_new_thread_core(s_glthread_core);
#endif
	const bool created = g_gs_device->Create(vsync && std::atoi(vsync) ? GSVSyncMode::FIFO : GSVSyncMode::Disabled, false);
#ifdef __SWITCH__
	ps2_switch_new_thread_core(-1);
#endif
	if (!created)
	{
		Console.Error("un5gs: the OpenGL GS device could not be created");
		g_gs_device.reset();
		return false;
	}
	g_gs_device->SetGPUTimingEnabled(true);
	g_gs_renderer = std::make_unique<GSRendererHW>();
	g_gs_renderer->SetRegsMem(s_privileged);
	g_gs_renderer->ResetPCRTC();
	g_gs_renderer->UpdateRenderFixes();
	return true;
}

void close()
{
#ifdef __SWITCH__
	if (s_clock_override)
	{
		const u32 lift[3] = {0, 0, 0};
		un5_hoc_override(lift);
		s_clock_override = false;
	}
#endif
	if (g_gs_renderer)
	{
		g_gs_renderer->Destroy();
		g_gs_renderer.reset();
	}
	if (g_gs_device)
	{
		g_gs_device->Destroy();
		g_gs_device.reset();
	}
}

void transfer(int path, const void* data, std::uint32_t qwords)
{
	GsTime timed;
	const u8* mem = static_cast<const u8*>(data);
	switch (path)
	{
		case 1: g_gs_renderer->Transfer<0>(mem, qwords); break;
		case 2: g_gs_renderer->Transfer<1>(mem, qwords); break;
		default: g_gs_renderer->Transfer<2>(mem, qwords); break;
	}
}

std::uint8_t* privileged_registers()
{
	return s_privileged;
}

void write_csr(std::uint32_t csr)
{
	g_gs_renderer->WriteCSR(csr);
}

void vsync(std::uint32_t field, bool registers_written)
{
	// As GSvsync in GS.cpp: the display rectangles first, then the pending draw.
	{
	GsTime timed;
	const Common::Timer::Value vsync_start = timed.start;
	struct VsyncTime
	{
		Common::Timer::Value start;
		~VsyncTime() { s_vsync_ticks += Common::Timer::GetCurrentValue() - start; }
	} vsync_timed{vsync_start};
	GSRenderer& r = *g_gs_renderer;
	r.PCRTCDisplays.SetVideoMode(r.GetVideoMode());
	r.PCRTCDisplays.EnableDisplays(r.m_regs->PMODE, r.m_regs->SMODE2, r.isReallyInterlaced());
	r.PCRTCDisplays.SetRects(0, r.m_regs->DISP[0].DISPLAY, r.m_regs->DISP[0].DISPFB);
	r.PCRTCDisplays.SetRects(1, r.m_regs->DISP[1].DISPLAY, r.m_regs->DISP[1].DISPFB);
	r.PCRTCDisplays.CheckSameSource();
	r.PCRTCDisplays.CalculateDisplayOffset(r.m_scanmask_used);
	r.PCRTCDisplays.CalculateFramebufferOffset(r.m_scanmask_used, r.m_regs->DISP[0].DISPFB, r.m_regs->DISP[1].DISPFB);
	r.Flush(GSState::VSYNC);
#ifdef __SWITCH__
	un5_osd_begin();
#endif
	r.VSync(field, registers_written, r.IsIdleFrame());
	}
#ifdef __SWITCH__
	un5_menu_poll();
#endif
	log_speed();
}

void read_fifo(void* data, std::uint32_t qwords)
{
	u8* mem = static_cast<u8*>(data);
	g_gs_renderer->InitReadFIFO(mem, qwords);
	g_gs_renderer->ReadFIFO(mem, qwords);
}

}
