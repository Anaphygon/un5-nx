// SPDX-License-Identifier: GPL-3.0+
// Runs uam on a thread of its own with an 8 MB stack, which Mesa's recursive
// GLSL compiler inside it needs; the GS thread's stack is small. Either the
// caller waits (60-80 ms a shader on a Switch: un5_uam_compile_big_stack), or
// the shader goes to the background compiler, one thread below the game's in
// priority that takes shaders one at a time (un5_uam_compile_async; the GS
// thread collects them with un5_uam_take_done). An abort inside uam logs a
// backtrace.
#include <switch.h>
#include <unwind.h>

#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

namespace
{
	struct AsyncJob
	{
		u64 hash;
		int stage;
		std::string glsl, out_path;
	};

	std::mutex g_lock;
	std::condition_variable g_wake;
	std::deque<AsyncJob> g_todo;
	std::vector<std::pair<u64, bool>> g_done;
	bool g_started = false, g_stop = false;
	Thread g_worker;
	void* g_worker_stack = nullptr;

	void work(void*)
	{
		void (*previous)(int) = std::signal(SIGABRT, on_abort);
		for (;;)
		{
			AsyncJob job;
			{
				std::unique_lock<std::mutex> lock(g_lock);
				g_wake.wait(lock, [] { return g_stop || !g_todo.empty(); });
				if (g_stop)
					break;
				job = std::move(g_todo.front());
				g_todo.pop_front();
			}
			const bool ok = un5_uam_compile(job.stage, job.glsl.c_str(), job.out_path.c_str());
			std::lock_guard<std::mutex> lock(g_lock);
			g_done.emplace_back(job.hash, ok);
		}
		std::signal(SIGABRT, previous);
	}
} // namespace

void un5_uam_compile_async(u64 hash, int stage, std::string glsl, std::string out_path)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (!g_started)
	{
		// The lowest priority an application's thread may take (0x3F is
		// refused): it runs when the game's threads wait. It lives until
		// un5_uam_stop.
		constexpr size_t kStack = 8 * 1024 * 1024;
		g_worker_stack = std::aligned_alloc(0x1000, kStack);
		Result rc = g_worker_stack ? threadCreate(&g_worker, work, nullptr, g_worker_stack, kStack, 0x3B, -2) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
		if (R_SUCCEEDED(rc))
			rc = threadStart(&g_worker);
		g_started = R_SUCCEEDED(rc);
		if (!g_started)
		{
			std::fprintf(stderr, "deko3d: could not start the background compiler (0x%x)\n", rc);
			g_done.emplace_back(hash, false);
			return;
		}
	}
	g_todo.push_back({hash, stage, std::move(glsl), std::move(out_path)});
	g_wake.notify_one();
}

bool un5_uam_take_done(u64& hash, bool& ok)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (g_done.empty())
		return false;
	hash = g_done.back().first;
	ok = g_done.back().second;
	g_done.pop_back();
	return true;
}

// Before the process exits (a thread left running breaks into hbl): the
// worker ends after the shader it is on, if any.
void un5_uam_stop()
{
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (!g_started)
			return;
		g_stop = true;
		g_todo.clear();
	}
	g_wake.notify_one();
	threadWaitForExit(&g_worker);
	threadClose(&g_worker);
	std::free(g_worker_stack);
	g_started = false;
}
