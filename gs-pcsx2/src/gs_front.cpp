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
#include "un5_settings.h"
#include "bench.h"
#ifdef __SWITCH__
#include "deko3d/GSDeviceDK.h"
#endif
#include "common/Console.h"
#include "common/Timer.h"
#include "fmt/format.h"

#include <algorithm>
#include <atomic>
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
bool un5_hoc_real_mhz(unsigned out[3]);
namespace EmuFolders { extern std::string Logs; } // pcsx2_glue.cpp
// From ps2-web-recomp's host/switch_threads.cpp: the core for threads
// created from now on (-1: libnx's default).
extern "C" void ps2_switch_new_thread_core(int core);
// ... and moves the last thread placed that way (glthread's worker).
extern "C" bool ps2_switch_move_placed_thread(int core);
// ps2-web-recomp gif.cpp: the body pass B merge forced on (1), off (0), or as
// the environment says (-1).
extern "C" void ps2_body_merge_override(int on);
int s_glthread_core = 1;
// The device in use is deko3d (UN5GS_RENDERER=dk), not OpenGL.
bool s_deko3d = false;
// Whether UN5's stage batches come here untransformed: with
// resources/shaders/opengl/tfx_un5_vgs.glsl (both devices compile it), without
// draw buffering; deko3d unless UN5GS_NATIVE=0, OpenGL with UN5GS_NATIVE=1.
bool s_native_batches = false;
// Whether the stand-in sends them now (UN5GS_SHOTS_AB turns it off for a while).
std::atomic<bool> s_native_send{true};
#endif

extern bool g_un5_honour_scanmsk; // pcsx2 GSState.cpp (patch 0006)
extern bool g_un5_dk_full_barrier; // deko3d/GSDeviceDK.cpp

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
//   UN5GS_HPO       half-pixel offset 0 off .. 5 (default 2, from PCSX2's game database)
//   UN5GS_NATIVE_SCALING  native scaling 0 off .. 4 (default 2, from the game database)
//   UN5GS_SCANMSK   1 honours the game's SCANMSK (draw every other line); default 0
//                   ignores it: UN5's ultimates left it set, and later frames kept
//                   half their lines from the ultimate (a stuck afterimage, stripes)
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
	// A multiple of the PS2's resolution: 1, 1.5, 2 or 3.
	const char* scale = std::getenv("UN5GS_UPSCALE");
	GSConfig.UpscaleMultiplier = std::clamp(scale ? static_cast<float>(std::atof(scale)) : 2.0f, 1.0f, 3.0f);
	GSConfig.LinearPresent = static_cast<GSPostBilinearMode>(std::clamp(number("UN5GS_FILTER", 2), 0, 2));
	// PCSX2's game database fixes for SLES-55605, which matter once the game is
	// upscaled: without them its full-screen effects (hit flashes, smoke) come
	// out in horizontal stripes. Half-pixel offset "Special", native scaling
	// "Aggressive".
	un5_apply_upscale_fix(std::clamp(number("UN5GS_FIX", 0), 0, kUpscaleFixes - 1));
	if (std::getenv("UN5GS_HPO"))
		GSConfig.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(std::clamp(number("UN5GS_HPO", 2), 0, 5));
	if (std::getenv("UN5GS_NATIVE_SCALING"))
		GSConfig.UserHacks_NativeScaling = static_cast<GSNativeScaling>(std::clamp(number("UN5GS_NATIVE_SCALING", 2), 0, 4));
	g_un5_honour_scanmsk = number("UN5GS_SCANMSK", 0) != 0;
	g_un5_dk_full_barrier = number("UN5DK_BARRIER", 0) != 0;
	Console.WriteLn("un5gs: texture preloading %d, blending %d, mipmap %d, dithering %d, download %d, upscale %.1fx, filter %d, "
					"half-pixel offset %d, native scaling %d",
		static_cast<int>(GSConfig.TexturePreloading), static_cast<int>(GSConfig.AccurateBlendingUnit), GSConfig.HWMipmap ? 1 : 0,
		GSConfig.Dithering, static_cast<int>(GSConfig.HWDownloadMode), GSConfig.UpscaleMultiplier, static_cast<int>(GSConfig.LinearPresent),
		static_cast<int>(GSConfig.UserHacks_HalfPixelOffset), static_cast<int>(GSConfig.UserHacks_NativeScaling));
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
// The last window's draws per frame and VU thread share, for the benchmark.
double s_last_draws = 0, s_last_vu_busy = 0;

void log_draw_split(u32 fields, double gs_ms)
{
	static const char* const kReasons[16] = {"unknown", "reset", "state change", "clut change", "gs transfer",
		"dirty texture", "dirty frame", "dirty z", "local move", "download", "save state", "load state",
		"autoflush", "vsync", "reopen", "vertex count"};
	const u64 draws = g_un5_split[0] + g_un5_split[1] + g_un5_split[2] + g_un5_split[3];
	const double per = fields ? 1.0 / fields : 0.0;
	s_last_draws = static_cast<double>(draws) * per;
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
	// Where a draw's time goes (pcsx2 GSState.h, Un5Span). Each stage holds
	// the ones after it: GIF transfers flush draws, a flush traces the
	// vertices and calls Draw(), Draw() looks up the texture and the targets
	// and calls DrawPrims(), which ends in RenderHW.
	const double* t = g_un5_gs_ms;
	Console.WriteLn("[gs-time] ms per frame: GIF transfers %.1f without their flushes (%.0f vertex kicks, CLUT loads %.1f); "
					"native batches %.1f without their flushes; flushes %.1f (from transfers %.1f, batches %.1f, others %.1f; "
					"vertex trace %.1f, outside Draw() %.1f); in Draw(): texture lookup %.1f, target lookups %.1f, DrawPrims %.1f "
					"(outside RenderHW %.1f), the rest %.1f",
		(t[5] - t[7]) * per, static_cast<double>(g_un5_kicks) * per, t[6] * per, (t[8] - t[9]) * per, t[4] * per, t[7] * per,
		t[9] * per, t[11] * per, t[3] * per, (t[4] - g_un5_draw_ms) * per, t[0] * per, t[1] * per, t[2] * per,
		(t[2] - g_un5_render_ms) * per, (g_un5_draw_ms - t[0] - t[1] - t[2]) * per);
	g_un5_kicks = 0;
	std::fill(std::begin(g_un5_gs_ms), std::end(g_un5_gs_ms), 0.0);
	s_vsync_ticks = 0;
	g_un5_swap_ms = 0.0;
	g_un5_gpu_behind = g_un5_presents = 0;
#ifdef __SWITCH__
	Console.WriteLn("[clocks] cpu %u (measured %u) gpu %u mem %u MHz; glthread worker on core %d", un5_clock_mhz(0),
		un5_measured_cpu_mhz(), un5_clock_mhz(1), un5_clock_mhz(2), s_glthread_core);
	// What the hardware really runs at, and how hot and hungry it is: a run on
	// a charger once measured 1833 MHz where 1963 was set, and dropped frames.
	unsigned real[3];
	int st[5];
	if (un5_hoc_real_mhz(real) && un5_hoc_status(st))
		Console.WriteLn("[thermal] real cpu %u gpu %u mem %u MHz; SoC %.1f, CPU %.1f, GPU %.1f C; %d mW (average %d)", real[0], real[1],
			real[2], st[0] / 10.0, st[1] / 10.0, st[2] / 10.0, st[3], st[4]);
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
	s_last_vu_busy = vu_busy;
	Console.WriteLn("[vu1] VU thread busy %.0f%%; %.1f interpreted program runs per frame", vu_busy,
		static_cast<double>(interpreted - s_interpreted) * per);
	s_interpreted = interpreted;
	std::fill(std::begin(g_un5_flush_reason), std::end(g_un5_flush_reason), 0);
	std::fill(std::begin(g_un5_split), std::end(g_un5_split), 0);
	g_un5_indices = 0;
	g_un5_draw_ms = g_un5_render_ms = 0.0;
}

// The PCRTC registers whenever they change, with the field they changed on
// (UN5 sets them once: both circuits a line apart, its flicker filter).
void log_display_registers()
{
	static u32 s_field = 0;
	static u64 s_last[10] = {};
	const GSPrivRegSet& regs = *g_gs_renderer->m_regs;
	const u64 now[10] = {regs.PMODE.U64, regs.SMODE2.U64, regs.DISP[0].DISPFB.U64, regs.DISP[0].DISPLAY.U64,
		regs.DISP[1].DISPFB.U64, regs.DISP[1].DISPLAY.U64, regs.EXTBUF.U64, regs.EXTDATA.U64, regs.EXTWRITE.U64,
		regs.BGCOLOR.U64};
	++s_field;
	if (std::equal(std::begin(now), std::end(now), std::begin(s_last)))
		return;
	std::copy(std::begin(now), std::end(now), std::begin(s_last));
	Console.WriteLn("[pcrtc] field %u: PMODE %llx SMODE2 %llx DISPFB1 %llx DISPLAY1 %llx DISPFB2 %llx DISPLAY2 %llx "
					"EXTBUF %llx EXTDATA %llx EXTWRITE %llx BGCOLOR %llx",
		s_field, now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9]);
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
	if (s_native_batches)
	{
		GSState::UN5Stats& st = g_gs_renderer->m_un5_stats;
		Console.WriteLn("[stage] per frame: %.0f native batches (%.0f with nothing to draw yet, %.0f restarting a strip), %.0f draws, %.0f triangles",
			st.batches / static_cast<double>(s_fields), st.empty / static_cast<double>(s_fields), st.rechained / static_cast<double>(s_fields),
			st.draws / static_cast<double>(s_fields), st.triangles / static_cast<double>(s_fields));
		st = {};
	}
	// UN5GS_UPSCALE_CYCLE="2,1": the internal resolution steps through the list
	// every 10 s (as PCSX2 changes it from its settings), to compare in one run.
	static const char* s_upscale_cycle = std::getenv("UN5GS_UPSCALE_CYCLE");
	static u32 s_upscale_windows = 0;
	if (s_upscale_cycle && ++s_upscale_windows % 2 == 0)
	{
		std::vector<int> scales;
		for (const char* p = s_upscale_cycle; *p;)
		{
			scales.push_back(std::clamp(std::atoi(p), 1, 3));
			const char* comma = std::strchr(p, ',');
			p = comma ? comma + 1 : p + std::strlen(p);
		}
		const int next = scales[(s_upscale_windows / 2) % scales.size()];
		if (next != static_cast<int>(GSConfig.UpscaleMultiplier))
		{
			GSConfig.UpscaleMultiplier = static_cast<float>(next);
			g_gs_renderer->PurgeTextureCache(true, true, true);
			g_gs_device->ClearCurrent();
			g_gs_device->PurgePool();
		}
		Console.WriteLn("[gs] upscale now %dx", next);
	}
	// UN5GS_NATIVE_CYCLE=1: the stage batches alternate between the GPU and
	// VU1 every 10 s, to compare them in one run.
	static const bool s_native_cycle = std::getenv("UN5GS_NATIVE_CYCLE") != nullptr;
	static u32 s_native_windows = 0;
	if (s_native_batches && s_native_cycle && ++s_native_windows % 2 == 0)
	{
		s_native_send = !s_native_send.load(std::memory_order_relaxed);
		Console.WriteLn("[stage] batches now %s", s_native_send.load(std::memory_order_relaxed) ? "on the GPU" : "on VU1");
	}
	// UN5DK_BARRIER_CYCLE=1: deko3d's hazard barrier alternates between
	// fragments and full every 30 s, to compare them in one run.
	static const bool s_barrier_cycle = std::getenv("UN5DK_BARRIER_CYCLE") != nullptr;
	static u32 s_windows = 0;
	if (s_barrier_cycle && ++s_windows % 6 == 0)
	{
		g_un5_dk_full_barrier = !g_un5_dk_full_barrier;
		Console.WriteLn("[dk] hazard barrier now %s", g_un5_dk_full_barrier ? "full" : "fragments");
	}
	// The launcher's Benchmark: the next window's variant.
	if (un5_bench_on())
	{
		const double gs_ms = s_fields ? Common::Timer::ConvertValueToMilliseconds(s_gs_ticks) / s_fields : 0.0;
		const Un5BenchChoice next = un5_bench_window({s_fields / seconds, gs_ms, s_last_vu_busy, s_last_draws});
		if (s_native_batches)
			s_native_send = next.gpu_stage;
		ps2_body_merge_override(next.body_merge ? 1 : 0);
	}
	s_shader_compiles = 0;
	g_un5_program_ms = 0.0;
	s_start = now;
	s_fields = 0;
	s_gs_ticks = 0;
}

// PCSX2 logs through Console; the runner's log is stderr, unbuffered, on the
// SD card. Developer messages are kept to 10 a second: the texture cache warns
// "Uneven pages mess up" on every draw of some UN5 effects, up to 1500 lines a
// session, and writing them stalled the GS thread for up to a second.
void log_to_stderr(LOGLEVEL level, ConsoleColors, std::string_view message)
{
	if (message.find("Compiling new") != std::string_view::npos)
	{
		++s_shader_compiles;
		return;
	}
	if (level >= LOGLEVEL_DEV)
	{
		static Common::Timer::Value second = 0;
		static u32 written = 0, dropped = 0;
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (Common::Timer::ConvertValueToSeconds(now - second) >= 1.0)
		{
			if (dropped)
				std::fprintf(stderr, "[log] %u developer messages dropped\n", dropped);
			second = now;
			written = dropped = 0;
		}
		if (++written > 10)
		{
			++dropped;
			return;
		}
	}
	std::fwrite(message.data(), 1, message.size(), stderr);
	if (message.empty() || message.back() != '\n')
		std::fputc('\n', stderr);
}
}

#ifdef __SWITCH__
extern "C" void util_queue_kill_all_threads(void); // our Mesa (patches/switch-mesa/0003)
// Defined with the stage program's stand-in (ports/un5/vu1_native.cpp), which
// sends its batches untransformed while this says yes.
extern "C" bool (*un5_native_batches_hook)();
#endif

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
#ifdef __SWITCH__
	// UN5GS_RENDERER=dk draws with deko3d (src/deko3d/) instead of OpenGL.
	const char* renderer = std::getenv("UN5GS_RENDERER");
	s_deko3d = renderer && std::strcmp(renderer, "dk") == 0;
	if (s_deko3d)
		g_gs_device = std::make_unique<GSDeviceDK>();
	else
#endif
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
		Console.Error("un5gs: the %s GS device could not be created", s_deko3d ? "deko3d" : "OpenGL");
		g_gs_device.reset();
		return false;
	}
	g_gs_device->SetGPUTimingEnabled(true);
	g_gs_renderer = std::make_unique<GSRendererHW>();
	g_gs_renderer->SetRegsMem(s_privileged);
	g_gs_renderer->ResetPCRTC();
	g_gs_renderer->UpdateRenderFixes();
	const char* native = std::getenv("UN5GS_NATIVE");
	const bool shader = s_deko3d ? static_cast<GSDeviceDK*>(g_gs_device.get())->UN5ShaderLoaded() :
								   static_cast<GSDeviceOGL*>(g_gs_device.get())->UN5ShaderLoaded();
	// On by default with deko3d; with OpenGL only for UN5GS_NATIVE=1: from one
	// Switch run on, every OpenGL frame showed zoomed in, cause unknown.
	const bool wanted = s_deko3d ? !(native && std::strcmp(native, "0") == 0) : (native && std::strcmp(native, "1") == 0);
	s_native_batches = shader && wanted && !GSConfig.UserHacks_DrawBuffering;
	un5_native_batches_hook = [] { return s_native_batches && s_native_send.load(std::memory_order_relaxed); };
	Console.WriteLn("un5gs: stage batches %s", s_native_batches ? "transformed on the GPU" : "transformed by VU1");
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
#ifdef __SWITCH__
	// Mesa's worker threads (glthread and others) would outlive the runner's
	// _Exit, which skips the atexit handler that stops them, and hbl breaks
	// on a thread alive at exit (seen 2026-10-29 after Quit from the menu).
	util_queue_kill_all_threads();
#endif
}

// A batch of UN5's stage program sent untransformed (ports/un5/vu1_native.cpp):
// a PACKED tag of one NOP register, then the "UN5N" header.
static bool is_native_batch(const u8* mem, std::uint32_t qwords)
{
	if (qwords < 12)
		return false;
	u32 tag[4], info[4];
	std::memcpy(tag, mem, sizeof(tag));
	std::memcpy(info, mem + 16, sizeof(info));
	return tag[1] == 0x10000000u && tag[2] == 0xFu && tag[3] == 0 && (tag[0] & 0xFFFF7FFFu) + 1 == qwords && info[0] == 0x4E354E55u && info[1] == 1 &&
		   info[2] + 11 == qwords;
}

void transfer(int path, const void* data, std::uint32_t qwords)
{
	GsTime timed;
	const u8* mem = static_cast<const u8*>(data);
	if (path == 1 && s_native_batches && is_native_batch(mem, qwords))
	{
		g_gs_renderer->UN5NativeBatch(mem);
		return;
	}
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

// UN5GS_SHOTS=n,n,...: the presented picture of those vsyncs (counted from
// open) as half-size PPM files next to the log, shot_<n>.ppm, for checking a
// bench run. Each one stalls that frame (a readback and ~3 MB to the card).
// With UN5GS_SHOTS_AB=1 the stage batches go through VU1 for the 20 vsyncs
// before each n, and a second shot at n + 20 has them back on the GPU: the
// same scene both ways, for comparing.
static void take_shots()
{
	static const char* list = std::getenv("UN5GS_SHOTS");
	static const bool ab = std::getenv("UN5GS_SHOTS_AB") && std::getenv("UN5GS_SHOTS_AB")[0] == '1';
	static u32 count = 0;
	++count;
	if (!list)
		return;
	for (const char* p = list; *p;)
	{
		char* end;
		const unsigned long n = std::strtoul(p, &end, 10);
		if (end == p)
			break;
		if (ab && count == n - 20)
			s_native_send = false;
		if (ab && count == n)
			s_native_send = true;
		if (n == count || (ab && n + 20 == count))
		{
			u32 w = 0, h = 0;
			std::vector<u32> pixels;
			if (!g_gs_renderer->SaveSnapshotToMemory(0, 0, false, false, &w, &h, &pixels) || !w || !h)
				return;
			// Half size and one write: the SD card is slow with small ones.
			const u32 hw = w / 2, hh = h / 2;
			const std::string header = fmt::format("P6\n{} {}\n255\n", hw, hh);
			std::vector<u8> file(header.begin(), header.end());
			file.reserve(header.size() + hw * hh * 3);
			for (u32 y = 0; y < hh; y++)
			{
				for (u32 x = 0; x < hw; x++)
				{
					const u32 c = pixels[(y * 2) * w + x * 2];
					file.push_back(c & 0xFF);
					file.push_back((c >> 8) & 0xFF);
					file.push_back((c >> 16) & 0xFF);
				}
			}
			const std::string path = fmt::format("{}/shot_{}.ppm", ::EmuFolders::Logs, count);
			if (std::FILE* f = std::fopen(path.c_str(), "wb"))
			{
				std::fwrite(file.data(), 1, file.size(), f);
				std::fclose(f);
				Console.WriteLn("un5gs: shot of vsync %u (%ux%u) in %s", count, hw, hh, path.c_str());
			}
			return;
		}
		p = *end ? end + 1 : end;
	}
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
	log_display_registers();
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
	take_shots();
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

const char* un5_upscale_fix_name(int fix)
{
	static const char* const kNames[kUpscaleFixes] = {"Game database", "Native scaling normal", "Native scaling off",
		"Half-pixel native", "Round sprite", "Deinterlace adaptive", "NetherSX2"};
	return kNames[std::clamp(fix, 0, kUpscaleFixes - 1)];
}

void un5_apply_upscale_fix(int fix)
{
	// The game database's fixes for SLES-55605 are the starting point.
	GSConfig.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Special;
	GSConfig.UserHacks_NativeScaling = GSNativeScaling::Aggressive;
	GSConfig.UserHacks_RoundSprite = 0;
	GSConfig.UserHacks_CPUCLUTRender = 0;
	GSConfig.InterlaceMode = GSInterlaceMode::Automatic;
	switch (fix)
	{
		case 1: GSConfig.UserHacks_NativeScaling = GSNativeScaling::Normal; break;
		case 2: GSConfig.UserHacks_NativeScaling = GSNativeScaling::Off; break;
		case 3: GSConfig.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Native; break;
		case 4: GSConfig.UserHacks_RoundSprite = 2; break;
		case 5: GSConfig.InterlaceMode = GSInterlaceMode::AdaptiveTFF; break;
		case 6:
			// NetherSX2's game database for SLES-55605 (its emucore_4248 build,
			// from an older PCSX2 that has no native scaling): half-pixel offset
			// normal and round sprite full. Its third fix, CPU CLUT render, is
			// left out: here it read render targets back from the GPU up to 32
			// times a frame and stalled battles to ~30 fps.
			GSConfig.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Normal;
			GSConfig.UserHacks_NativeScaling = GSNativeScaling::Off;
			GSConfig.UserHacks_RoundSprite = 2;
			break;
		default: break;
	}
	Console.WriteLn("un5gs: upscale fix %s", un5_upscale_fix_name(fix));
}
