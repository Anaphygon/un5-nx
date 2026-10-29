// SPDX-License-Identifier: GPL-3.0+
// The name a GS shader has in the deko3d shader pack: 64-bit FNV-1a of its
// GLSL text, exactly as the OpenGL device would compile it.
#pragma once

#include "common/Pcsx2Types.h"

#include <string_view>

inline u64 un5_shader_hash(std::string_view text)
{
	u64 hash = 0xcbf29ce484222325ull;
	for (const char c : text)
	{
		hash ^= static_cast<u8>(c);
		hash *= 0x100000001b3ull;
	}
	return hash;
}
