// SPDX-License-Identifier: GPL-3.0+
// un5-nx's front end to PCSX2's GS, in place of PCSX2's GS.cpp (which belongs
// to its VM: settings, hotkeys, captures). It opens the OpenGL device and the
// hardware renderer and forwards what the runtime's GIF unit sees.
// Needs proper testing on hardware: the flow follows GS.cpp, trimmed.
#include "un5gs.h"

#include "GS/GS.h"
#include "GS/GSState.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/OpenGL/GSDeviceOGL.h"
#include "common/Console.h"

Pcsx2Config::GSOptions GSConfig;

// Raw storage in emu_config_storage.cpp; see there.
static_assert(sizeof(Pcsx2Config) <= (1 << 16));

namespace {
alignas(16) u8 s_privileged[0x2000];
}

namespace un5gs {

bool open()
{
	GSConfig.Renderer = GSRendererType::OGL;
	EmuConfig.CurrentAspectRatio = AspectRatioType::R4_3;
	EmuConfig.CurrentCustomAspectRatio = 0.0f;
	g_gs_device = std::make_unique<GSDeviceOGL>();
	if (!g_gs_device->Create(GSVSyncMode::FIFO, false))
	{
		Console.Error("un5gs: the OpenGL GS device could not be created");
		g_gs_device.reset();
		return false;
	}
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

void read_fifo(void* data, std::uint32_t qwords)
{
	u8* mem = static_cast<u8*>(data);
	g_gs_renderer->InitReadFIFO(mem, qwords);
	g_gs_renderer->ReadFIFO(mem, qwords);
}

}
