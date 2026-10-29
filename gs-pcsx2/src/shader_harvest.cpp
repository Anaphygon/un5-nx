// SPDX-License-Identifier: GPL-3.0+
// Keeps the source of every program the OpenGL device compiles, one file per
// stage named by a hash of its text, so the deko3d device's shader pack can be
// built from what the game really uses: the deko3d device makes the same text,
// hashes it the same way, and loads <hash>.dksh (tools/dk_shaders.py compiles
// the harvest with uam). Each source is written once.
#include "deko3d/dk_shader_hash.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_set>

#include <sys/stat.h>

namespace
{
	constexpr const char* kDir = "sdmc:/switch/un5-nx/shaders-gl";
	std::unordered_set<u64> s_seen;

	void keep(std::string_view source, const char* stage)
	{
		const u64 hash = un5_shader_hash(source);
		if (!s_seen.insert(hash).second)
			return;
		char path[96];
		std::snprintf(path, sizeof path, "%s/%016llx.%s", kDir, static_cast<unsigned long long>(hash), stage);
		struct stat st;
		if (stat(path, &st) == 0)
			return;
		if (std::FILE* f = std::fopen(path, "wb"))
		{
			std::fwrite(source.data(), 1, source.size(), f);
			std::fclose(f);
		}
	}
} // namespace

void un5_shader_harvest(std::string_view vertex_shader, std::string_view fragment_shader)
{
	static const bool s_ready = [] {
		mkdir(kDir, 0777);
		return true;
	}();
	(void)s_ready;
	keep(vertex_shader, "vert");
	keep(fragment_shader, "frag");
}
