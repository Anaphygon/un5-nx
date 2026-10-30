// Renders the launcher to PPM files on a desktop, with its own drawing code:
//   make -C gs-pcsx2/preview && gs-pcsx2/preview/launcher-preview <out dir>
// One picture per button press in the script below. The console font is
// replaced by UN5_PREVIEW_FONT (default: a macOS system font).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "switch.h"

bool un5_launcher(const char* dir);

namespace
{
	u32 g_fb[1280 * 720];
	void* g_font;
	std::string g_out;
	int g_frame;
	// Each step is one frame's buttons; a picture is saved after each.
	const std::vector<u64> kScript = {0, HidNpadButton_AnyUp, HidNpadButton_AnyUp, HidNpadButton_AnyUp,
		HidNpadButton_AnyRight, HidNpadButton_AnyUp, HidNpadButton_AnyUp, HidNpadButton_AnyUp, HidNpadButton_AnyUp,
		HidNpadButton_AnyUp, HidNpadButton_Plus};
	size_t g_step;
} // namespace

const char* un5_upscale_fix_name(int fix)
{
	static const char* const kNames[] = {"Game database", "Native scaling normal", "Native scaling off", "Half-pixel native",
		"Round sprite", "Deinterlace adaptive"};
	return kNames[fix];
}
bool un5_hoc_open() { return true; }
u32 un5_hoc_nearest_hz(u32, u32 mhz) { return mhz * 1000000; }
bool un5_hoc_override(const u32*) { return true; }
const char* un5_clock_service_name() { return "Horizon OC"; }

void padConfigureInput(u32, u32) {}
void padInitializeAny(PadState* p) { *p = {}; }
void padUpdate(PadState* p) { p->down = g_step < kScript.size() ? kScript[g_step] : HidNpadButton_Plus; }
bool appletMainLoop() { return g_step++ <= kScript.size(); }
void svcSleepThread(s64) {}
NWindow* nwindowGetDefault() { static NWindow w; return &w; }
Result framebufferCreate(Framebuffer*, NWindow*, u32, u32, u32, u32) { return 0; }
Result framebufferMakeLinear(Framebuffer*) { return 0; }
void* framebufferBegin(Framebuffer*, u32* stride) { *stride = 1280 * 4; return g_fb; }
void framebufferEnd(Framebuffer*)
{
	char path[512];
	std::snprintf(path, sizeof path, "%s/launcher_%02d.ppm", g_out.c_str(), g_frame++);
	if (std::FILE* f = std::fopen(path, "wb"))
	{
		std::fprintf(f, "P6\n1280 720\n255\n");
		for (u32 px : g_fb)
		{
			const unsigned char rgb[3] = {static_cast<unsigned char>(px), static_cast<unsigned char>(px >> 8),
				static_cast<unsigned char>(px >> 16)};
			std::fwrite(rgb, 1, 3, f);
		}
		std::fclose(f);
	}
}
void framebufferClose(Framebuffer*) {}
Result plInitialize(PlServiceType) { return 0; }
void plExit() {}
Result plGetSharedFontByType(PlFontData* font, PlSharedFontType t)
{
	if (t != PlSharedFontType_Standard)
		return 1; // no NintendoExt on a desktop
	const char* path = std::getenv("UN5_PREVIEW_FONT");
	std::FILE* f = std::fopen(path ? path : "/System/Library/Fonts/Supplemental/Arial.ttf", "rb");
	if (!f)
		return 1;
	std::fseek(f, 0, SEEK_END);
	const long n = std::ftell(f);
	std::rewind(f);
	g_font = std::malloc(n);
	std::fread(g_font, 1, n, f);
	std::fclose(f);
	font->address = g_font;
	font->size = static_cast<u32>(n);
	return 0;
}

int main(int argc, char** argv)
{
	g_out = argc > 1 ? argv[1] : ".";
	un5_launcher("/tmp/un5-preview-");
	std::printf("%d pictures in %s\n", g_frame, g_out.c_str());
	return 0;
}
