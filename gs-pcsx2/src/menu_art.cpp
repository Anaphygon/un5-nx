// SPDX-License-Identifier: GPL-3.0+
#include "menu_art.h"

#include <algorithm>

namespace
{
	constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xFF000000u | (b << 16) | (g << 8) | r; }

	constexpr int kW = 1280, kH = 720;
	constexpr uint32_t kPaper = rgb(218, 214, 207), kInk = rgb(24, 21, 20), kInkSoft = rgb(84, 78, 74), kGrey = rgb(146, 143, 140),
					   kOrange = rgb(252, 168, 44), kGlow = rgb(70, 214, 64), kBrush = rgb(128, 34, 26), kTitle = rgb(255, 200, 60),
					   kTitleEdge = rgb(150, 24, 30), kLight = rgb(236, 232, 226), kWhite = rgb(255, 255, 255);
	constexpr int kListTop = 170, kListBottom = 600, kListX = 112, kValueRight = 610;
	constexpr int kPanelX = 676, kPanelY = 170, kPanelW = 524, kPanelH = 430;

	// A repeatable hash, for the paper's grain and the brush's ragged edges.
	uint32_t noise(uint32_t x, uint32_t y)
	{
		uint32_t h = x * 374761393u + y * 668265263u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return h ^ (h >> 16);
	}

	// Smooth noise in 0..1 along x, one knot every `period` pixels.
	float wave(int x, int period, uint32_t seed)
	{
		const int k = x / period;
		const float t = static_cast<float>(x % period) / period, s = t * t * (3 - 2 * t);
		const float a = (noise(k, seed) & 1023) / 1023.0f, b = (noise(k + 1, seed) & 1023) / 1023.0f;
		return a + (b - a) * s;
	}

	void paper(Un5Painter& p)
	{
		p.rect(0, 0, kW, kH, kPaper);
		constexpr int grain = 8;
		for (int y = 0; y < kH; y += grain)
			for (int x = 0; x < kW; x += grain)
			{
				const uint32_t n = noise(x / grain, y / grain) & 63;
				if (n < 12)
					p.shade(x, y, grain, grain, kInk, 6 + n / 2);
				else if (n > 58)
					p.shade(x, y, grain, grain, kWhite, 24);
			}
		// Darker towards the sides, like old paper.
		for (int i = 0; i < 12; ++i)
		{
			p.shade(i * 10, 0, 10, kH, kInk, (12 - i) * 4);
			p.shade(kW - 10 - i * 10, 0, 10, kH, kInk, (12 - i) * 4);
		}
	}

	// The torn ink bands at the top and the bottom of the game's screen.
	void ink_band(Un5Painter& p, bool top, int depth)
	{
		const uint32_t seed = top ? 11 : 23;
		for (int x = 0; x < kW; x += 2)
		{
			const int d = static_cast<int>(depth * (0.5f + 0.3f * wave(x, 140, seed) + 0.14f * wave(x, 23, seed + 1) +
													0.08f * wave(x, 5, seed + 2)));
			p.rect(x, top ? 0 : kH - d, 2, d, kInk);
		}
		// Dry-brush streaks where the paper shows through.
		for (int i = 0; i < 18; ++i)
		{
			const uint32_t n = noise(i, seed + 3);
			const int x = static_cast<int>(n % kW), w = 40 + static_cast<int>((n >> 11) % 260);
			const int off = 6 + static_cast<int>((n >> 21) % (depth / 2));
			p.shade(x, top ? off : kH - off, w, 2, kPaper, 120);
		}
	}

	// The red brush stroke under the chosen item: thick on the left, a tip on the right.
	void brush(Un5Painter& p, int x, int cy, int w, int h)
	{
		for (int i = 0; i < w; i += 2)
		{
			const float t = static_cast<float>(i) / w;
			float thick = h * (t < 0.05f ? 0.55f + 9.0f * t : 1.0f - 0.6f * (t - 0.05f) / 0.95f);
			thick *= 0.88f + 0.24f * wave(i, 17, 31);
			const int y = cy - static_cast<int>(thick / 2) + static_cast<int>(3 * wave(i, 60, 37)) + i / 60;
			p.rect(x + i, y, 2, static_cast<int>(thick), kBrush);
			if ((noise(i / 2, 41) & 7) == 0)
				p.shade(x + i, y + static_cast<int>(thick * wave(i, 9, 43)), 6, 2, kPaper, 90);
		}
	}

	// Text with an edge r pixels wide around it, as the game's menu letters.
	void outlined(Un5Painter& p, int x, int y, int px, uint32_t fill, uint32_t edge, int r, const char* t)
	{
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
				if ((dx || dy) && dx * dx + dy * dy <= r * r + 1)
					p.text(x + dx, y + dy, px, edge, t);
		p.text(x, y, px, fill, t);
		p.text(x + 1, y, px, fill, t);
	}

	std::string upper(std::string t)
	{
		for (char& c : t)
			if (c >= 'a' && c <= 'z')
				c = static_cast<char>(c - 'a' + 'A');
		return t;
	}

	void wrapped(Un5Painter& p, int x, int y, int px, uint32_t c, const std::string& text, int max_w, int line_h)
	{
		std::string line;
		size_t at = 0;
		while (at <= text.size())
		{
			size_t end = text.find(' ', at);
			if (end == std::string::npos)
				end = text.size();
			const std::string word = text.substr(at, end - at);
			const std::string tried = line.empty() ? word : line + " " + word;
			if (!line.empty() && p.width(px, tried.c_str()) > max_w)
			{
				p.text(x, y, px, c, line.c_str());
				y += line_h;
				line = word;
			}
			else
				line = tried;
			at = end + 1;
		}
		if (!line.empty())
			p.text(x, y, px, c, line.c_str());
	}

	void item(Un5Painter& p, int y, bool sel, const Un5MenuItem& it)
	{
		const std::string caps = upper(it.label);
		if (sel)
		{
			brush(p, kListX - 46, y + 19, kValueRight - kListX + 90, 40);
			outlined(p, kListX, y, 28, kOrange, kGlow, 4, caps.c_str());
			outlined(p, kListX, y, 28, kOrange, kInk, 1, caps.c_str());
		}
		else
			outlined(p, kListX, y, 28, kGrey, kInkSoft, 2, caps.c_str());
		if (it.value.empty())
			return;
		const int vx = kValueRight - p.width(20, it.value.c_str());
		if (sel)
			outlined(p, vx, y + 7, 20, kLight, kInk, 1, it.value.c_str());
		else
			p.text(vx, y + 7, 20, kInkSoft, it.value.c_str());
	}

	void panel(Un5Painter& p, const Un5MenuScreen& s)
	{
		p.shade(kPanelX, kPanelY, kPanelW, kPanelH, kInk, 26);
		p.rect(kPanelX, kPanelY, kPanelW, 3, kInk);
		p.rect(kPanelX, kPanelY + kPanelH - 3, kPanelW, 3, kInk);
		const int x = kPanelX + 32;
		outlined(p, x, kPanelY + 28, 30, kGrey, kInkSoft, 2, upper(s.heading).c_str());
		outlined(p, x, kPanelY + 84, 40, kOrange, kInk, 2, s.value.c_str());
		p.rect(x, kPanelY + 150, kPanelW - 64, 2, kInkSoft);
		wrapped(p, x, kPanelY + 172, 24, kInk, s.note, kPanelW - 64, 34);
	}
} // namespace

void un5_draw_menu(Un5Painter& p, const Un5MenuScreen& s)
{
	paper(p);
	ink_band(p, true, 64);
	ink_band(p, false, 70);
	outlined(p, 84, 36, 24, kOrange, kTitleEdge, 2, "NARUTO SHIPPUDEN");
	outlined(p, 84, 66, 46, kTitle, kTitleEdge, 3, "ULTIMATE NINJA 5");
	p.text(88, 126, 18, kInkSoft, s.tagline);
	const int n = static_cast<int>(s.items.size());
	const int step = n > 1 ? std::min(48, (kListBottom - kListTop - 36) / (n - 1)) : 0;
	for (int i = 0; i < n; ++i)
		item(p, kListTop + i * step, i == s.selected, s.items[i]);
	panel(p, s);
	p.text(kW - 60 - p.width(22, s.hints.c_str()), kH - 44, 22, kLight, s.hints.c_str());
}
