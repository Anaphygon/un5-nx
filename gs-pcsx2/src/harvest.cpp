// SPDX-License-Identifier: GPL-3.0+
// The Shader harvest's button presses. harvest-select.state (made on the Mac
// from a save with all 62 characters unlocked) is Practice's Character Select
// at frame 6130, cursor on Naruto, chakra set to unlimited. The run has one
// round per character (--rounds): each loads that state again, so a long
// loading in one round cannot shift the next, steps the cursor to its
// character the short way round (each of the grid's two rows is 31 long and
// wraps), picks it and its link for both sides, and fights until the round
// ends: combos, jutsu, ultimate with mashing, steps, jumps, throws. Battle! is
// pressed four times: for some characters the VS screen took the first one too
// early, and the jutsu's triangle then went back to Mode Select. The same
// presses were checked frame by frame on the Mac.
#include "harvest.h"

#include <cstdio>
#include <string>

namespace
{
	constexpr int kStateFrame = 6130;
	constexpr int kRowLength = 31, kCharacters = 2 * kRowLength;
	constexpr int kFightFrames = 1500; // 50 a second
	// The longest cursor walk, the picks, the loading, the fight, a margin.
	constexpr int kRoundFrames = (kRowLength / 2 + 1) * 40 + 20 + 700 + 1200 + kFightFrames + 200;

	struct Timeline
	{
		std::FILE* f;
		int t;

		void press(const char* button, int gap, int hold = 4)
		{
			std::fprintf(f, "--press\n%d:%s:%d\n", t, button, hold);
			t += gap;
		}
	};

	void round(Timeline& tl, int character, int end)
	{
		const int row = character / kRowLength, i = character % kRowLength;
		if (row)
			tl.press("down", 40);
		for (int step = 0; step < (i <= kRowLength / 2 ? i : kRowLength - i); ++step)
			tl.press(i <= kRowLength / 2 ? "right" : "left", 40);
		for (int step = 0; step < 6; ++step) // 1P, its link, link mode; COM, its link, link mode
			tl.press("cross", 100);
		for (int step = 0; step < 4; ++step) // Battle!: the VS screen can be slow to take it; later ones jump
			tl.press("cross", 100);
		for (int n = 0; tl.t < end - 400; ++n)
		{
			for (int hit = 0; hit < 4; ++hit)
				tl.press("circle", 12);
			tl.t += 12;
			tl.press("triangle", 12); // jutsu
			tl.press("circle", 60);
			tl.press("triangle", 12); // ultimate
			tl.press("triangle", 12);
			tl.press("circle", 40);
			for (int mash = 0; mash < 10; ++mash)
				tl.press("circle", 8);
			tl.press(n % 2 ? "left" : "right", 40, 16);
			tl.press("cross", 40);
			tl.press("square", 60);
		}
	}
} // namespace

bool write_harvest_args(const std::string& path)
{
	std::FILE* f = std::fopen(path.c_str(), "w");
	if (!f)
		return false;
	std::fprintf(f, "# written by the launcher's Shader harvest\n"
					"--window\n--fullscreen\n--disc\n$DIR/disc.iso\n--memcard\n$DIR/%s\n--load-state\n$DIR/%s\n",
		kHarvestCard, kHarvestState);
	for (int c = 0; c < kCharacters; ++c)
	{
		Timeline tl{f, kStateFrame + c * kRoundFrames + 20};
		round(tl, c, kStateFrame + (c + 1) * kRoundFrames - 60);
	}
	std::fprintf(f, "--rounds\n%d\n--frames\n%d\n", kRoundFrames, kStateFrame + kCharacters * kRoundFrames);
	std::fclose(f);
	return true;
}
