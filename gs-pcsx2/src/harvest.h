// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <string>

// The launcher's Shader harvest: every character's combos, jutsu and
// ultimate in Practice, so the deko3d shaders their effects need are built
// (and kept in the pack) before the game is played.
inline constexpr const char* kHarvestState = "harvest-select.state"; // Practice Character Select, all unlocked
inline constexpr const char* kHarvestCard = "harvest.ps2";           // that save's memory card

// Writes the run as args.txt would give it; false if the file cannot be written.
bool write_harvest_args(const std::string& path);
