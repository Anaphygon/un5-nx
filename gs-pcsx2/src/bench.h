// SPDX-License-Identifier: GPL-3.0+
#pragma once

// What a 5 s speed window measured (gs_front.cpp's log_speed).
struct Un5BenchWindow
{
	double fps, gs_ms, vu_busy, draws;
};

// UN5GS_BENCH=1, set by the launcher's Benchmark.
bool un5_bench_on();
// Counts a finished window; writes UN5_BENCH_RESULT each time.
void un5_bench_window(const Un5BenchWindow& w);
