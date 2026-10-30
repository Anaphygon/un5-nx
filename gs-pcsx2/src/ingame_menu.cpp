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
#include "menu_art.h"
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
		// The menu's paper and ink run past 64K vertices; both devices draw with the offset.
		io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
		ImFontConfig config;
		config.FontDataOwnedByAtlas = false;
		io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(font), static_cast<int>(size), 30.0f, &config);
		return true;
	}

	constexpr int kResume = kSettingRows, kQuit = kSettingRows + 1, kRowCount = kSettingRows + 2;

	// Drawn like the launcher (menu_art.cpp), in 1280x720 units scaled to the screen.
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

	class ImGuiPainter : public Un5Painter
	{
	public:
		explicit ImGuiPainter(const Canvas& c) : m_c(c) {}
		void rect(int x, int y, int w, int h, uint32_t c) override { m_c.dl->AddRectFilled(m_c.at(x, y), m_c.at(x + w, y + h), c); }
		void shade(int x, int y, int w, int h, uint32_t c, int a) override
		{
			m_c.dl->AddRectFilled(m_c.at(x, y), m_c.at(x + w, y + h), (c & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24));
		}
		void text(int x, int y, int px, uint32_t c, const char* t) override { m_c.text(x, y, px, c, t); }
		int width(int px, const char* t) override { return static_cast<int>(m_c.width(px, t)); }

	private:
		const Canvas& m_c;
	};

	void draw(int selected, const Un5Settings& s)
	{
		const Canvas c = begin_frame();
		Un5MenuScreen screen{"Paused  \xc2\xb7  changes take effect when you resume", {}, selected, {}, {}, {}, {}};
		for (int r = 0; r < kSettingRows; ++r)
		{
			const Un5RowText t = un5_row_text(s, r, true);
			screen.items.push_back({t.label, t.value});
		}
		screen.items.push_back({"Resume", ""});
		screen.items.push_back({"Quit game", ""});
		if (selected == kResume)
		{
			screen.heading = "Paused";
			screen.value = "Resume";
			screen.note = "Back to the game, with the settings on the left. Resolution and renderer change at the next start.";
		}
		else if (selected == kQuit)
		{
			screen.heading = "Leave";
			screen.value = "Quit game";
			screen.note = "Ends the game and gives the clocks back to the clock service. Save in the game first.";
		}
		else
		{
			const Un5RowText t = un5_row_text(s, selected, true);
			screen.heading = t.label;
			screen.value = "<  " + t.value + "  >";
			screen.note = t.note;
		}
		screen.hints = "Up/Down choose    Left/Right change    A select    B resume";
		ImGuiPainter painter(c);
		un5_draw_menu(painter, screen);
	}

	// The settings a resume puts in force at once (resolution waits for a restart).
	void apply_live(const Un5Settings& s)
	{
		GSConfig.AccurateBlendingUnit = s.min_blending ? AccBlendLevel::Minimum : AccBlendLevel::Basic;
		GSConfig.LinearPresent = static_cast<GSPostBilinearMode>(s.filter);
		ps2::set_audio_stretch(s.audio_stretch);
		un5_apply_upscale_fix(s.upscale_fix);
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
			selected = (selected + kRowCount - 1) % kRowCount;
		if (down & kDown)
			selected = (selected + 1) % kRowCount;
		const int dx = (down & kRight) ? 1 : (down & kLeft) ? -1 : 0;
		if (selected >= kResume)
		{
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
