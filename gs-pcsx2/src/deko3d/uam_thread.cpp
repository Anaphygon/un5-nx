// SPDX-License-Identifier: GPL-3.0+
// Runs uam on a thread of its own with an 8 MB stack, which Mesa's recursive
// GLSL compiler inside it needs; the GS thread's stack is small. The caller
// waits (60-80 ms a shader on a Switch). An abort inside uam logs a backtrace.
#include <switch.h>
#include <unwind.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>

extern "C" bool un5_uam_compile(int stage, const char* glsl, const char* out_path);

bool un5_uam_compile_big_stack(int stage, const char* glsl, const char* out_path);

namespace
{
	struct Job
	{
		int stage;
		const char* glsl;
		const char* out_path;
		bool ok;
	};

	_Unwind_Reason_Code trace_frame(_Unwind_Context* ctx, void* arg)
	{
		int* depth = static_cast<int*>(arg);
		std::fprintf(stderr, "  [uam abort] frame %d: pc 0x%lx\n", *depth, static_cast<unsigned long>(_Unwind_GetIP(ctx)));
		return ++*depth < 24 ? _URC_NO_REASON : _URC_END_OF_STACK;
	}

	// An abort inside uam: print where (subtract the base below for addr2line
	// against ps2run.elf) before the process goes.
	void on_abort(int)
	{
		std::fprintf(stderr, "deko3d: uam aborted; un5_uam_compile_big_stack is at 0x%lx\n",
			reinterpret_cast<unsigned long>(&un5_uam_compile_big_stack));
		int depth = 0;
		_Unwind_Backtrace(trace_frame, &depth);
		std::fflush(stderr);
		std::_Exit(1);
	}

	void run(void* arg)
	{
		Job* job = static_cast<Job*>(arg);
		void (*previous)(int) = std::signal(SIGABRT, on_abort);
		job->ok = un5_uam_compile(job->stage, job->glsl, job->out_path);
		std::signal(SIGABRT, previous);
	}
} // namespace

bool un5_uam_compile_big_stack(int stage, const char* glsl, const char* out_path)
{
	// libnx's own thread call with a stack we allocate (pthread's large-stack
	// path took the emulator down before the thread ran a line).
	constexpr size_t kStack = 8 * 1024 * 1024;
	void* stack = std::aligned_alloc(0x1000, kStack);
	if (!stack)
		return false;
	Job job{stage, glsl, out_path, false};
	s32 prio = 0x2C;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	Thread thread;
	Result rc = threadCreate(&thread, run, &job, stack, kStack, prio, -2);
	if (R_SUCCEEDED(rc))
	{
		rc = threadStart(&thread);
		if (R_SUCCEEDED(rc))
			threadWaitForExit(&thread);
		threadClose(&thread);
	}
	std::free(stack);
	if (R_FAILED(rc))
		std::fprintf(stderr, "deko3d: could not start the uam thread (0x%x)\n", rc);
	return R_SUCCEEDED(rc) && job.ok;
}
