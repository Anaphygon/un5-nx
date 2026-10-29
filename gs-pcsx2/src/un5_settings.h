// SPDX-License-Identifier: GPL-3.0+
// The player's settings, shared by the launcher (launcher.cpp, before the
// game) and the in-game menu (ingame_menu.cpp, L3+R3). Both show the same
// rows; this file has no libnx or PCSX2 types so either side can include it.
#pragma once

#include <string>

struct Un5Settings
{
	int preset = 0;      // index into the performance presets; 0 leaves the clocks alone
	int upscale = 2;     // internal resolution, 1 native .. 3 (2x: sharper, and Sharp is cheap there)
	int filter = 2;      // screen filter: 0 off, 1 smooth, 2 sharp
	bool min_blending = true;
	bool audio_stretch = true;
	bool show_fps = false;
};

enum Un5Row { kRowPreset, kRowUpscale, kRowFilter, kRowBlending, kRowAudio, kRowFps, kSettingRows };

struct Un5RowText
{
	const char* label;
	std::string value, note;
};

// What a row shows; in_game notes that a new resolution waits for the next start.
Un5RowText un5_row_text(const Un5Settings& s, int row, bool in_game);
// Moves a row's value by step (left -1, right or A +1).
void un5_row_step(Un5Settings& s, int row, int step);

// The settings in force, and a change made in the game: clocks applied now,
// everything kept in launcher.ini for the next start. The GS and audio parts
// are applied by the caller.
Un5Settings un5_settings();
void un5_settings_change(const Un5Settings& s);
