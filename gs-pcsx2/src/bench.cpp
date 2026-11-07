// SPDX-License-Identifier: GPL-3.0+
// The launcher's Benchmark: a Practice fight (no timer, no KO) replayed with
// the settings the game is played with, its 5 s speed windows written to
// bench-result.txt for the launcher to show next time. After the counted
// windows the sampling profiler starts, for where the frame goes.
#include "bench.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef __SWITCH__
extern "C" void ps2_sampler_go(); // the runner's sampling profiler (host/switch_sampler.cpp)
#endif

namespace
{
	struct Sum
	{
		int windows = 0, full_speed = 0;
		double fps = 0, min_fps = 1e9, gs_ms = 0, vu_busy = 0, draws = 0;

		void add(const Un5BenchWindow& w)
		{
			++windows;
			full_speed += w.fps >= kFullSpeed;
			fps += w.fps;
			min_fps = std::min(min_fps, w.fps);
			gs_ms += w.gs_ms;
			vu_busy += w.vu_busy;
			draws += w.draws;
		}
		static constexpr double kFullSpeed = 49.0; // PAL: 50 fields a second
	};

	// The first window holds the state load. Then 30 windows (150 s) are
	// counted; the rest of the run is sampled and not counted, since the
	// sampler slows every thread.
	constexpr int kSkippedWindows = 1;
	constexpr int kCountedWindows = 30;

	int g_seen = 0;
	Sum g_counted, g_sampled;

	void write_result(const char* path)
	{
		std::FILE* f = std::fopen(path, "w");
		if (!f)
			return;
		const std::time_t now = std::time(nullptr);
		char when[32];
		std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&now));
		const char* scale = std::getenv("UN5GS_UPSCALE");
		const Sum& s = g_counted;
		// A summary for the launcher's screen, a blank line, then the details.
		std::fprintf(f, "Last run %s, %sx:\n", when, scale ? scale : "?");
		std::fprintf(f, "%.1f fps on average, %.1f in the slowest 5 s\n", s.fps / s.windows, s.min_fps);
		std::fprintf(f, "full speed %d%% of the time\n", 100 * s.full_speed / s.windows);
		std::fprintf(f, "\n%d windows of 5 s: GS %.1f ms/frame, VU thread %.0f%% busy, %.0f draws/frame\n", s.windows, s.gs_ms / s.windows,
			s.vu_busy / s.windows, s.draws / s.windows);
		if (g_sampled.windows)
			std::fprintf(f, "then %d windows under the sampler: %.1f fps\n", g_sampled.windows, g_sampled.fps / g_sampled.windows);
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

void un5_bench_window(const Un5BenchWindow& w)
{
	if (++g_seen <= kSkippedWindows)
		return;
	const bool sampling = g_counted.windows >= kCountedWindows;
	(sampling ? g_sampled : g_counted).add(w);
	if (const char* path = std::getenv("UN5_BENCH_RESULT"))
		write_result(path);
	if (g_counted.windows == kCountedWindows && !sampling)
	{
#ifdef __SWITCH__
		ps2_sampler_go();
#endif
		std::fprintf(stderr, "[bench] %d windows counted; sampling the rest\n", kCountedWindows);
	}
}
