// Stands in for GSRendererHWMultiISA.cpp. That file lets the hardware renderer
// draw some sprites with PCSX2's software rasteriser, whose ARM64 code
// generator needs vixl; without it, those draws take the hardware path.
#include "GS/Renderers/HW/GSRendererHW.h"

class CURRENT_ISA::GSRendererHWFunctions
{
public:
	static void Populate(GSRendererHW& renderer)
	{
		renderer.SwPrimRender = [](GSRendererHW&, bool, bool) { return false; };
	}
};

MULTI_ISA_UNSHARED_IMPL;

void CURRENT_ISA::GSRendererHWPopulateFunctions(GSRendererHW& renderer)
{
	GSRendererHWFunctions::Populate(renderer);
}
