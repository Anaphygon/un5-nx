// SPDX-License-Identifier: GPL-3.0+
// The in-game menu: L3+R3 (both sticks pressed) pauses the game and shows the
// launcher's settings over the last frame, drawn with ImGui through PCSX2's
// own overlay path. The game stops by itself while the render thread stays
// in here (the queues to it fill and the emulation threads wait).
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "common/Timer.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "un5_settings.h"

#include <cfloat>
#include <cstdio>
#include <string>
#include <thread>

void un5_pad_read(u32& held, u32& down);
bool un5_shared_font(const void** data, std::size_t* size);
bool un5_hoc_override(const u32 hz[3]);
const char* un5_clock_service_name();
namespace ps2
{
	void request_stop();
	void set_audio_stretch(bool on);
} // namespace ps2

namespace
{
	constexpr u32 kA = 1, kB = 2, kUp = 4, kDown = 8, kLeft = 16, kRight = 32, kL3 = 64, kR3 = 128;
	ImGuiContext* s_context = nullptr;

	void wait_release()
	{
		for (u32 held = ~0u, down = 0; held;)
		{
			un5_pad_read(held, down);
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}
	}

	bool make_context()
	{
		if (s_context)
			return true;
		const void* font = nullptr;
		std::size_t size = 0;
		if (!un5_shared_font(&font, &size))
			return false;
		s_context = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		// GSDevice::UpdateImGuiTextures creates and fills the font atlas.
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
		ImFontConfig config;
		config.FontDataOwnedByAtlas = false;
		io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(font), static_cast<int>(size), 30.0f, &config);
		return true;
	}

	constexpr int kResume = kSettingRows, kQuit = kSettingRows + 1, kRowCount = kSettingRows + 2;

	// Drawn like the launcher (launcher.cpp), in 1280x720 units scaled to the
	// screen: tiles, a cyan-to-green frame on the selection, notes under the
	// values.
	ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

	struct Canvas
	{
		ImDrawList* dl;
		ImFont* font;
		float k, ox, oy;
		ImVec2 at(float x, float y) const { return ImVec2(ox + x * k, oy + y * k); }
		void text(float x, float y, float px, ImU32 c, const char* t) const { dl->AddText(font, px * k, at(x, y), c, t); }
		float width(float px, const char* t) const { return font->CalcTextSizeA(px * k, FLT_MAX, 0.0f, t).x / k; }
	};

	Canvas begin_frame()
	{
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(static_cast<float>(g_gs_device->GetWindowWidth()), static_cast<float>(g_gs_device->GetWindowHeight()));
		io.DeltaTime = 1.0f / 60.0f;
		// A frame the GS never presented (it skips some) is ended here.
		if (GImGui->FrameCountEnded != GImGui->FrameCount)
			ImGui::EndFrame();
		ImGui::NewFrame();
		const float k = io.DisplaySize.x / 1280.0f;
		return Canvas{ImGui::GetForegroundDrawList(), ImGui::GetFont(), k, (io.DisplaySize.x - 1280.0f * k) * 0.5f,
			(io.DisplaySize.y - 720.0f * k) * 0.5f};
	}

	void draw(int selected, const Un5Settings& s)
	{
		const Canvas c = begin_frame();
		const ImU32 bg = rgb(16, 30, 39, 238), tile = rgb(18, 36, 48), tile_sel = rgb(22, 47, 66), line = rgb(52, 78, 92),
					fg = rgb(240, 244, 246), muted = rgb(138, 158, 170), cyan = rgb(90, 210, 225), green = rgb(100, 245, 156),
					accent = rgb(255, 150, 60);
		c.dl->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, bg);
		c.text(140, 34, 36, fg, "Paused");
		c.text(140, 84, 20, muted, "un5-nx  \xc2\xb7  changes take effect when you resume");
		c.dl->AddRectFilled(c.at(30, 130), c.at(1250, 131), line);
		const auto frame = [&](float x, float y, float w, float h) {
			c.dl->AddRectFilled(c.at(x - 4, y - 4), c.at(x + w + 4, y), cyan);
			c.dl->AddRectFilled(c.at(x - 4, y + h), c.at(x + w + 4, y + h + 4), green);
			c.dl->AddRectFilledMultiColor(c.at(x - 4, y), c.at(x, y + h), cyan, cyan, green, green);
			c.dl->AddRectFilledMultiColor(c.at(x + w, y), c.at(x + w + 4, y + h), cyan, cyan, green, green);
		};
		for (int r = 0; r < kSettingRows; ++r)
		{
			const Un5RowText t = un5_row_text(s, r, true);
			const float y = 146.0f + r * 70.0f;
			const bool sel = r == selected;
			c.dl->AddRectFilled(c.at(140, y), c.at(1140, y + 62), sel ? tile_sel : tile);
			if (sel)
				frame(140, y, 1000, 62);
			c.text(170, y + 7, 21, sel ? cyan : fg, t.label);
			const std::string v = "<  " + t.value + "  >";
			c.text(1110 - c.width(21, v.c_str()), y + 7, 21, fg, v.c_str());
			c.text(170, y + 37, 16, muted, t.note.c_str());
		}
		const auto button = [&](int index, float x, float w, const char* label, ImU32 fill) {
			const bool sel = index == selected;
			c.dl->AddRectFilled(c.at(x, 574), c.at(x + w, 630), sel ? fill : tile, 12 * c.k);
			if (sel)
				frame(x, 574, w, 56);
			c.text(x + w / 2 - c.width(26, label) / 2, 586, 26, sel ? rgb(20, 20, 20) : fg, label);
		};
		button(kResume, 140, 640, "Resume", accent);
		button(kQuit, 800, 340, "Quit game", rgb(255, 96, 86));
		c.dl->AddRectFilled(c.at(30, 646), c.at(1250, 647), line);
		c.text(80, 668, 18, muted, "Quit ends the game and gives the clocks back to the clock service.");
		const char* hints = "A choose   B resume";
		c.text(1200 - c.width(24, hints), 664, 24, fg, hints);
	}

	// The settings a resume puts in force at once (resolution waits for a restart).
	void apply_live(const Un5Settings& s)
	{
		GSConfig.AccurateBlendingUnit = s.min_blending ? AccBlendLevel::Minimum : AccBlendLevel::Basic;
		GSConfig.LinearPresent = static_cast<GSPostBilinearMode>(s.filter);
		ps2::set_audio_stretch(s.audio_stretch);
	}

	bool s_show_fps = false, s_fps_read = false;
} // namespace

// Called from vsync on the render thread, before the frame is presented: the
// FPS counter (UN5_SHOW_FPS, or the menu's setting) is drawn into it.
void un5_osd_begin()
{
	if (!s_fps_read)
	{
		const char* e = std::getenv("UN5_SHOW_FPS");
		s_show_fps = e && std::atoi(e) != 0;
		s_fps_read = true;
	}
	if (!s_show_fps || !g_gs_device || !make_context())
		return;
	static Common::Timer::Value s_since = Common::Timer::GetCurrentValue();
	static u32 s_count = 0;
	static float s_fps = 0.0f;
	++s_count;
	const double seconds = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_since);
	if (seconds >= 0.5)
	{
		s_fps = static_cast<float>(s_count / seconds);
		s_count = 0;
		s_since = Common::Timer::GetCurrentValue();
	}
	ImGui::SetCurrentContext(s_context);
	const Canvas c = begin_frame();
	char text[32];
	std::snprintf(text, sizeof text, "%.0f fps", s_fps);
	c.dl->AddRectFilled(c.at(14, 10), c.at(14 + c.width(22, text) + 16, 42), rgb(0, 0, 0, 150), 6 * c.k);
	c.text(22, 13, 22, s_fps >= 48.0f ? rgb(100, 245, 156) : s_fps >= 35.0f ? rgb(255, 200, 90) : rgb(255, 96, 86), text);
}

// Called from vsync on the render thread after the present: opens the menu on L3+R3.
void un5_menu_poll()
{
	u32 held = 0, down = 0;
	un5_pad_read(held, down);
	if ((held & (kL3 | kR3)) != (kL3 | kR3) || !(down & (kL3 | kR3)) || !g_gs_renderer || !make_context())
		return;
	wait_release();
	Un5Settings s = un5_settings();
	s.show_fps = s_show_fps;
	int selected = kResume;
	bool quit = false;
	ImGui::SetCurrentContext(s_context);
	for (;;)
	{
		un5_pad_read(held, down);
		if (down & kB)
			break;
		if (down & kUp)
			selected = selected == kQuit ? kSettingRows - 1 : (selected + kRowCount - 1) % kRowCount;
		if (down & kDown)
			selected = selected == kResume ? 0 : (selected + 1) % kRowCount;
		const int dx = (down & kRight) ? 1 : (down & kLeft) ? -1 : 0;
		if (selected >= kResume)
		{
			if (dx)
				selected = selected == kResume ? kQuit : kResume;
			if (down & kA)
			{
				quit = selected == kQuit;
				break;
			}
		}
		else if (dx || (down & kA))
			un5_row_step(s, selected, dx ? dx : 1);
		draw(selected, s);
		g_gs_renderer->PresentCurrentFrame(); // the frozen frame, with the menu drawn over it
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	apply_live(s);
	s_show_fps = s.show_fps;
	un5_settings_change(s);
	if (quit)
	{
		const u32 lift[3] = {0, 0, 0};
		un5_hoc_override(lift);
		ps2::request_stop();
	}
	wait_release();
	// With no current context the GS draws no overlay (the FPS counter makes
	// it current again each frame it is on).
	if (GImGui && GImGui->FrameCountEnded != GImGui->FrameCount)
		ImGui::EndFrame();
	ImGui::SetCurrentContext(nullptr);
}
