// SPDX-License-Identifier: GPL-3.0+
// The launcher: a settings screen before the game starts, from the homebrew
// menu (it is skipped when args.txt is there, for benchmark runs). The
// choices are kept in launcher.ini next to the NRO and take effect through the
// environment the runner and the GS read, and through Horizon OC's clock
// override for the performance presets.
#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "launcher_gfx.h"
#include "un5_settings.h"

bool un5_hoc_open();
u32 un5_hoc_nearest_hz(u32 module, u32 mhz);
bool un5_hoc_override(const u32 hz[3]);
const char* un5_clock_service_name();

namespace
{
	constexpr Color kBg = RGB(16, 30, 39), kTile = RGB(18, 36, 48), kTileSel = RGB(22, 47, 66), kLine = RGB(52, 78, 92),
					kText = RGB(240, 244, 246), kMuted = RGB(138, 158, 170), kCyan = RGB(90, 210, 225), kGreen = RGB(100, 245, 156),
					kAccent = RGB(255, 150, 60);

	struct Preset
	{
		const char* name;
		u32 cpu, gpu, mem; // MHz; 0 leaves the clocks to the clock service
		const char* note;
	};
	// Battle frame rates measured on a Mariko Switch at native resolution (docs/PHASE4.md).
	constexpr Preset kPresets[] = {
		{"Leave as they are", 0, 0, 0, "The clocks stay as Horizon OC or sys-clk set them."},
		{"Battery", 1224, 768, 1600, "CPU 1224, GPU 768, RAM 1600 MHz: about 32 fps in battle."},
		{"Balanced", 1785, 921, 1996, "CPU 1785, GPU 921, RAM 1996 MHz: about 44 fps in battle."},
		{"Fast", 1963, 921, 1996, "CPU 1963, GPU 921, RAM 1996 MHz: about 47-48 fps in battle."},
	};
	constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

	std::string g_dir;
	Un5Settings g_current;

	std::string ini_path(const char* dir) { return std::string(dir) + "launcher.ini"; }

	Un5Settings load(const char* dir)
	{
		Un5Settings s;
		if (std::FILE* f = std::fopen(ini_path(dir).c_str(), "r"))
		{
			char line[128];
			while (std::fgets(line, sizeof line, f))
			{
				int v = 0;
				if (std::sscanf(line, "preset=%d", &v) == 1 && v >= 0 && v < kPresetCount)
					s.preset = v;
				else if (std::sscanf(line, "upscale=%d", &v) == 1 && v >= 1 && v <= 3)
					s.upscale = v;
				else if (std::sscanf(line, "filter=%d", &v) == 1 && v >= 0 && v <= 2)
					s.filter = v;
				else if (std::sscanf(line, "min_blending=%d", &v) == 1)
					s.min_blending = v != 0;
				else if (std::sscanf(line, "audio_stretch=%d", &v) == 1)
					s.audio_stretch = v != 0;
				else if (std::sscanf(line, "show_fps=%d", &v) == 1)
					s.show_fps = v != 0;
			}
			std::fclose(f);
		}
		return s;
	}

	void save(const Un5Settings& s)
	{
		if (g_dir.empty())
			return;
		if (std::FILE* f = std::fopen(ini_path(g_dir.c_str()).c_str(), "w"))
		{
			std::fprintf(f, "preset=%d\nupscale=%d\nfilter=%d\nmin_blending=%d\naudio_stretch=%d\nshow_fps=%d\n", s.preset, s.upscale,
				s.filter, s.min_blending ? 1 : 0, s.audio_stretch ? 1 : 0, s.show_fps ? 1 : 0);
			std::fclose(f);
		}
	}

	void set_clocks(int preset)
	{
		if (!un5_hoc_open())
			return;
		const Preset& p = kPresets[preset];
		const u32 hz[3] = {p.cpu ? un5_hoc_nearest_hz(0, p.cpu) : 0, p.cpu ? un5_hoc_nearest_hz(1, p.gpu) : 0,
			p.cpu ? un5_hoc_nearest_hz(2, p.mem) : 0};
		if (un5_hoc_override(hz))
			std::fprintf(stderr, "[settings] clocks %s: %u/%u/%u MHz\n", p.name, hz[0] / 1000000, hz[1] / 1000000, hz[2] / 1000000);
	}

	constexpr int kStart = kSettingRows; // the launcher's last row
	constexpr int kRowsTop = 146, kRowStep = 70, kRowH = 62;

	void row(int index, int selected, const Un5RowText& t)
	{
		const int y = kRowsTop + index * kRowStep;
		const bool sel = index == selected;
		gfx_rect(140, y, 1000, kRowH, sel ? kTileSel : kTile);
		if (sel)
			gfx_border(136, y - 4, 1008, kRowH + 8, 4, kCyan, kGreen);
		gfx_text(170, y + 7, 21, sel ? kCyan : kText, t.label);
		const std::string v = std::string("<  ") + t.value + "  >";
		gfx_text(1110 - gfx_text_width(21, v.c_str()), y + 7, 21, kText, v.c_str());
		gfx_text_fit(170, y + 37, 16, kMuted, t.note.c_str(), 940);
	}

	void draw(const Un5Settings& s, int selected)
	{
		gfx_begin();
		gfx_clear(kBg);
		gfx_text(140, 34, 36, kText, "Naruto Shippuden: Ultimate Ninja 5");
		gfx_text(140, 84, 20, kMuted, "un5-nx  ·  native port  ·  settings before the game starts");
		gfx_rect(30, 130, 1220, 1, kLine);
		for (int r = 0; r < kSettingRows; ++r)
			row(r, selected, un5_row_text(s, r, false));
		const bool sel = selected == kStart;
		gfx_round(140, 574, 1000, 56, 12, sel ? kAccent : kTile);
		if (sel)
			gfx_border(136, 570, 1008, 64, 4, kCyan, kGreen);
		const char* start = "Start game";
		gfx_text(640 - gfx_text_width(26, start) / 2, 586, 26, sel ? RGB(20, 20, 20) : kText, start);
		gfx_rect(30, 646, 1220, 1, kLine);
		gfx_text_fit(80, 668, 18, kMuted, "L3+R3 in the game opens these settings; its Quit gives the clocks back. Closed from HOME, they stay until changed.", 820);
		std::string hints = std::string(GLYPH_A[0] ? GLYPH_A : "A") + " Start   " + (GLYPH_PLUS[0] ? GLYPH_PLUS : "+") + " Quit";
		gfx_text(1200 - gfx_text_width(24, hints.c_str()), 664, 24, kText, hints.c_str());
		gfx_end();
	}
} // namespace

Un5RowText un5_row_text(const Un5Settings& s, int row, bool in_game)
{
	static const char* const kFilters[] = {"Off", "Smooth", "Sharp"};
	switch (row)
	{
		case kRowPreset:
		{
			const Preset& p = kPresets[s.preset];
			const char* service = un5_clock_service_name();
			std::string note = p.note;
			if (!service)
				note = "No clock service is running (Horizon OC or sys-clk), so the clocks stay as they are.";
			else if (p.cpu)
				note += std::string("  Set through ") + service + (service[0] == 's' ? ", within its own limits." : ".");
			return {"Performance", p.name, note};
		}
		case kRowUpscale:
			return {"Resolution", s.upscale == 1 ? "Native" : std::to_string(s.upscale) + "x",
				std::string(s.upscale == 1 ? "The PS2's own resolution: fastest, soft on a sharp screen." :
											  "Renders the game larger and sharper; more work for the GPU.") +
					(in_game ? "  Takes effect next time the game starts." : "")};
		case kRowFilter:
			return {"Screen filter", kFilters[s.filter],
				s.filter == 0 ? "No smoothing: the sharpest, blocky edges." :
				s.filter == 1 ? "PCSX2's default smoothing: soft." :
				s.upscale == 1 ? "Bilinear sharp: crisp, but at Native it costs about 6 ms a frame; use it with 2x or more." :
								 "Bilinear sharp: smooth scaling, crisper than Smooth."};
		case kRowFps:
			return {"FPS counter", s.show_fps ? "On" : "Off",
				s.show_fps ? "Frames per second in a corner of the screen; costs well under a millisecond." :
							 "No counter on screen."};
		case kRowBlending:
			return {"Blending", s.min_blending ? "Minimum" : "Basic",
				s.min_blending ? "Faster: about 1 ms less GPU work per frame." : "PCSX2's default accuracy, a little slower."};
		default:
			return {"Audio stretching", s.audio_stretch ? "On" : "Off",
				s.audio_stretch ? "Repeats a few ms of sound when the game falls behind, instead of crackling." :
								  "Sound as the game makes it; gaps crackle when the game falls behind."};
	}
}

void un5_row_step(Un5Settings& s, int row, int step)
{
	switch (row)
	{
		case kRowPreset:
			if (un5_clock_service_name())
				s.preset = (s.preset + kPresetCount + step) % kPresetCount;
			break;
		case kRowUpscale: s.upscale = (s.upscale - 1 + 3 + step) % 3 + 1; break;
		case kRowFilter: s.filter = (s.filter + 3 + step) % 3; break;
		case kRowBlending: s.min_blending = !s.min_blending; break;
		case kRowAudio: s.audio_stretch = !s.audio_stretch; break;
		case kRowFps: s.show_fps = !s.show_fps; break;
	}
}

Un5Settings un5_settings() { return g_current; }

void un5_settings_change(const Un5Settings& s)
{
	const bool clocks = s.preset != g_current.preset;
	g_current = s;
	save(s);
	if (clocks)
		set_clocks(s.preset);
}

// Shows the launcher; false when the player quits instead of starting.
bool un5_launcher(const char* dir)
{
	g_dir = dir;
	if (!gfx_init())
	{
		gfx_exit();
		return true; // no screen to draw on: start with the defaults
	}
	Un5Settings s = load(dir);
	if (!un5_clock_service_name())
		s.preset = 0;
	PadState pad;
	padConfigureInput(8, HidNpadStyleSet_NpadStandard);
	padInitializeAny(&pad);
	int selected = kStart;
	bool start = false, dirty = true;
	while (appletMainLoop())
	{
		padUpdate(&pad);
		const u64 down = padGetButtonsDown(&pad);
		if (down & HidNpadButton_Plus)
			break;
		if (down & HidNpadButton_AnyUp)
			selected = (selected + kStart) % (kStart + 1), dirty = true;
		if (down & HidNpadButton_AnyDown)
			selected = (selected + 1) % (kStart + 1), dirty = true;
		const int dx = (down & HidNpadButton_AnyRight) ? 1 : (down & HidNpadButton_AnyLeft) ? -1 : 0;
		if (selected == kStart && (down & HidNpadButton_A))
			start = true;
		else if (dx || (down & HidNpadButton_A))
			un5_row_step(s, selected, dx ? dx : 1), dirty = true;
		if (start)
			break;
		if (dirty)
		{
			draw(s, selected);
			dirty = false;
		}
		svcSleepThread(16'000'000);
	}
	// The framebuffer gives the window back before the GS opens it with EGL.
	gfx_exit();
	if (!start)
		return false;
	g_current = s;
	save(s);
	setenv("UN5GS_BLEND", s.min_blending ? "0" : "1", 1);
	setenv("UN5GS_UPSCALE", std::to_string(s.upscale).c_str(), 1);
	setenv("UN5GS_FILTER", std::to_string(s.filter).c_str(), 1);
	setenv("PS2_AUDIO_STRETCH", s.audio_stretch ? "1" : "0", 1);
	setenv("UN5_SHOW_FPS", s.show_fps ? "1" : "0", 1);
	if (s.preset)
		set_clocks(s.preset);
	std::fprintf(stderr, "[launcher] preset %s, resolution %dx, filter %d, blending %s, audio stretching %s\n", kPresets[s.preset].name,
		s.upscale, s.filter, s.min_blending ? "minimum" : "basic", s.audio_stretch ? "on" : "off");
	return true;
}
