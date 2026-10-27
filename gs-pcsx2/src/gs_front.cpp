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

#include <algorithm>
#include <cstdio>
#include <cstdlib>

Pcsx2Config::GSOptions GSConfig;

// Raw storage in emu_config_storage.cpp; see there.
static_assert(sizeof(Pcsx2Config) <= (1 << 16));

// From patches/0003: time spent in GLShaderCache::GetProgram.
extern double g_un5_program_ms;

namespace {
alignas(16) u8 s_privileged[0x2000];

// GS settings from the environment (args.txt on the Switch), to trade
// accuracy for speed without a rebuild:
//   UN5GS_PRELOAD   texture preloading 0 off, 1 partial, 2 full (PCSX2 default)
//   UN5GS_BLEND     blending accuracy 0 minimum .. 5 maximum (default 1, basic)
//   UN5GS_MIPMAP    hardware mipmapping 0 or 1 (default 1)
//   UN5GS_DITHER    dithering 0 off .. 2 (default 2)
//   UN5GS_DOWNLOAD  GS-to-EE readbacks, PCSX2's GSHardwareDownloadMode: 0 enabled
//                   (default), 1 enabled and full, 2 no readbacks, 3 unsynchronized, 4 disabled
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
	GSConfig.AccurateBlendingUnit = static_cast<AccBlendLevel>(
		std::clamp(number("UN5GS_BLEND", static_cast<int>(GSConfig.AccurateBlendingUnit)), 0, 5));
	GSConfig.HWMipmap = number("UN5GS_MIPMAP", GSConfig.HWMipmap ? 1 : 0) != 0;
	GSConfig.Dithering = static_cast<u8>(std::clamp(number("UN5GS_DITHER", GSConfig.Dithering), 0, 2));
	GSConfig.HWDownloadMode = static_cast<GSHardwareDownloadMode>(
		std::clamp(number("UN5GS_DOWNLOAD", static_cast<int>(GSConfig.HWDownloadMode)), 0, 4));
	Console.WriteLn("un5gs: texture preloading %d, blending %d, mipmap %d, dithering %d, download %d", static_cast<int>(GSConfig.TexturePreloading),
		static_cast<int>(GSConfig.AccurateBlendingUnit), GSConfig.HWMipmap ? 1 : 0, GSConfig.Dithering, static_cast<int>(GSConfig.HWDownloadMode));
}

// Shader compiles, counted from PCSX2's log into the speed line.
u32 s_shader_compiles = 0;

// Time the render thread spends inside the GS, to tell a GS-bound frame from
// one that waits on emulation.
Common::Timer::Value s_gs_ticks = 0;

struct GsTime
{
	Common::Timer::Value start = Common::Timer::GetCurrentValue();
	~GsTime() { s_gs_ticks += Common::Timer::GetCurrentValue() - start; }
};

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
	if (!g_gs_device->Create(vsync && std::atoi(vsync) ? GSVSyncMode::FIFO : GSVSyncMode::Disabled, false))
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
	GSRenderer& r = *g_gs_renderer;
	r.PCRTCDisplays.SetVideoMode(r.GetVideoMode());
	r.PCRTCDisplays.EnableDisplays(r.m_regs->PMODE, r.m_regs->SMODE2, r.isReallyInterlaced());
	r.PCRTCDisplays.SetRects(0, r.m_regs->DISP[0].DISPLAY, r.m_regs->DISP[0].DISPFB);
	r.PCRTCDisplays.SetRects(1, r.m_regs->DISP[1].DISPLAY, r.m_regs->DISP[1].DISPFB);
	r.PCRTCDisplays.CheckSameSource();
	r.PCRTCDisplays.CalculateDisplayOffset(r.m_scanmask_used);
	r.PCRTCDisplays.CalculateFramebufferOffset(r.m_scanmask_used, r.m_regs->DISP[0].DISPFB, r.m_regs->DISP[1].DISPFB);
	r.Flush(GSState::VSYNC);
	r.VSync(field, registers_written, r.IsIdleFrame());
	}
	log_speed();
}

void read_fifo(void* data, std::uint32_t qwords)
{
	u8* mem = static_cast<u8*>(data);
	g_gs_renderer->InitReadFIFO(mem, qwords);
	g_gs_renderer->ReadFIFO(mem, qwords);
}

}
