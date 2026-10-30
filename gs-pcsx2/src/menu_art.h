// SPDX-License-Identifier: GPL-3.0+
// The look the launcher and the in-game menu share, after the game's own
// first screen (its language menu): paper, torn ink bands, outlined capitals,
// a red brush stroke under the chosen item, and the chosen item explained in
// a panel on the right. Drawn in 1280x720 units through a painter, so the
// launcher's software framebuffer and the in-game ImGui overlay share it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Colors are 0xAABBGGRR, as both the framebuffer and ImGui take them.
class Un5Painter
{
public:
	virtual ~Un5Painter() = default;
	virtual void rect(int x, int y, int w, int h, uint32_t c) = 0;
	// c blended at alpha a (0..255) over what is there.
	virtual void shade(int x, int y, int w, int h, uint32_t c, int a) = 0;
	// Text with its top-left at (x, y).
	virtual void text(int x, int y, int px, uint32_t c, const char* t) = 0;
	virtual int width(int px, const char* t) = 0;
};

struct Un5MenuItem
{
	std::string label, value; // an empty value marks an action (Start, Resume, Quit)
};

struct Un5MenuScreen
{
	const char* tagline;            // under the title
	std::vector<Un5MenuItem> items; // top to bottom
	int selected;
	// The panel on the right: a heading, the value in large type, then a note.
	std::string heading, value, note;
	std::string hints; // bottom right, on the ink
};

void un5_draw_menu(Un5Painter& p, const Un5MenuScreen& screen);
