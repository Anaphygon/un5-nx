// SPDX-License-Identifier: GPL-3.0+
// The launcher's Benchmark: the battle replay with the port's optional paths
// turned on and off by turns, one variant per 5 s window, and what each got
// written to bench-result.txt for the launcher to show next time. After the
// last counted window the sampling profiler starts, for where the frame goes.
#include "bench.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef __SWITCH__
extern "C" void ps2_sampler_go(); // the runner's sampling profiler (host/switch_sampler.cpp)
#endif

namespace
{
	struct Variant
	{
		const char* name;
		bool gpu_stage, body_merge;
	};
	constexpr std::array<Variant, 4> kVariants = {{
		{"GPU stage + merge", true, true},
		{"GPU stage", true, false},
		{"merge", false, true},
		{"neither", false, false},
	}};

	struct Sum
	{
		int windows = 0;
		double fps = 0, min_fps = 1e9, gs_ms = 0, vu_busy = 0, draws = 0;
	};

	// Windows with fewer draws are the load and the intro, not the battle.
	constexpr double kBattleDraws = 100.0;
	// Battle windows compared; the rest of the run is sampled under the first
	// variant, and not counted, since the sampler slows every thread.
	constexpr int kCountedWindows = 20;

	std::array<Sum, kVariants.size()> g_sums;
	int g_step = 0;       // battle windows so far
	int g_current = 0;    // the variant of the window under way
	Sum g_sampled;        // the windows under the sampler
	bool g_sampling = false;

	void write_result(const char* path)
	{
		std::FILE* f = std::fopen(path, "w");
		if (!f)
			return;
		const std::time_t now = std::time(nullptr);
		char when[32];
		std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&now));
		const char* scale = std::getenv("UN5GS_UPSCALE");
		// A summary for the launcher's screen, a blank line, then the details.
		std::fprintf(f, "Last run %s, %sx:\n", when, scale ? scale : "?");
		for (size_t v = 0; v < kVariants.size(); ++v)
			if (const Sum& s = g_sums[v]; s.windows)
				std::fprintf(f, "%.1f fps (low %.1f) %s\n", s.fps / s.windows, s.min_fps, kVariants[v].name);
		std::fprintf(f, "\n%d battle windows of 5 s\n", g_step);
		for (size_t v = 0; v < kVariants.size(); ++v)
			if (const Sum& s = g_sums[v]; s.windows)
				std::fprintf(f, "%s: %d windows, GS %.1f ms/frame, VU thread %.0f%% busy, %.0f draws/frame\n", kVariants[v].name,
					s.windows, s.gs_ms / s.windows, s.vu_busy / s.windows, s.draws / s.windows);
		if (const Sum& s = g_sampled; s.windows)
			std::fprintf(f, "then %d windows under the sampler (%s): %.1f fps\n", s.windows, kVariants[0].name, s.fps / s.windows);
		std::fclose(f);
	}
} // namespace

bool un5_bench_on()
{
	static const bool on = [] {
		const char* e = std::getenv("UN5GS_BENCH");
		return e && std::atoi(e) != 0;
	}();
	return on;
}

Un5BenchChoice un5_bench_window(const Un5BenchWindow& w)
{
	if (w.draws >= kBattleDraws)
	{
		Sum& s = g_sampling ? g_sampled : g_sums[g_current];
		++s.windows;
		s.fps += w.fps;
		s.min_fps = std::min(s.min_fps, w.fps);
		s.gs_ms += w.gs_ms;
		s.vu_busy += w.vu_busy;
		s.draws += w.draws;
		if (g_step < kCountedWindows)
			++g_step;
		if (const char* path = std::getenv("UN5_BENCH_RESULT"))
			write_result(path);
		if (g_step < kCountedWindows)
		{
			// Each round of four starts one variant later, so no variant always
			// gets the same moment of the fight.
			g_current = static_cast<int>((g_step + g_step / kVariants.size()) % kVariants.size());
			std::fprintf(stderr, "[bench] next window: %s\n", kVariants[g_current].name);
		}
		else if (!g_sampling)
		{
			g_sampling = true;
			g_current = 0;
#ifdef __SWITCH__
			ps2_sampler_go();
#endif
			std::fprintf(stderr, "[bench] comparison done; sampling under %s\n", kVariants[0].name);
		}
	}
	return {kVariants[g_current].gpu_stage, kVariants[g_current].body_merge};
}
