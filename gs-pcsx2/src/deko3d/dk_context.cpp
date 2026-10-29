// SPDX-License-Identifier: GPL-3.0+
#include "deko3d/dk_context.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

namespace dk
{
	namespace
	{
		void DebugCallback(void*, const char* context, DkResult result, const char* message)
		{
			Console.Error("deko3d: %s: %s (result %d)", context ? context : "?", message ? message : "?", static_cast<int>(result));
			unsigned long long addr = 0;
			if (message && std::sscanf(message, " Address: 0x%llx", &addr) == 1 && Context::s_instance)
				Context::s_instance->ExplainAddress(addr);
			std::fflush(stderr);
		}

		DkMemBlock MakeBlock(DkDevice device, u32 size, u32 flags)
		{
			DkMemBlockMaker maker;
			dkMemBlockMakerDefaults(&maker, device, (size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1));
			maker.flags = flags;
			return dkMemBlockCreate(&maker);
		}

		u32 AlignUp(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

		// A fence that never signals means the GPU stopped on a bad command;
		// waiting for it forever freezes the console. Give up with a clear
		// log line and a crash report instead.
		constexpr s64 kFenceTimeoutNs = 3'000'000'000;

		void WaitFence(DkQueue queue, DkFence* fence, u64 batch)
		{
			if (dkFenceWait(fence, kFenceTimeoutNs) == DkResult_Success)
				return;
			Console.Error("deko3d: batch %llu did not finish in 3 s (queue %s); stopping",
				static_cast<unsigned long long>(batch), dkQueueIsInErrorState(queue) ? "in error state" : "still busy");
			std::fflush(stderr);
			std::abort();
		}
	} // namespace

	Context* Context::s_instance = nullptr;

	void Context::Track(DkMemBlock block, std::string tag)
	{
		if (!block)
			return;
		if (m_blocks.size() > 16384)
		{
			// Forget the oldest freed blocks.
			m_blocks.erase(std::remove_if(m_blocks.begin(), m_blocks.begin() + 4096, [](const BlockInfo& b) { return b.freed != 0; }),
				m_blocks.begin() + 4096);
		}
		m_blocks.push_back({block, dkMemBlockGetGpuAddr(block), dkMemBlockGetSize(block), std::move(tag), m_current_batch, 0});
	}

	void Context::FreeBlock(DkMemBlock block)
	{
		if (!block)
			return;
		const u64 gpu = dkMemBlockGetGpuAddr(block);
		for (auto it = m_blocks.rbegin(); it != m_blocks.rend(); ++it)
		{
			if (it->gpu == gpu && it->freed == 0)
			{
				it->freed = m_current_batch;
				break;
			}
		}
		Defer([block]() { dkMemBlockDestroy(block); });
	}

	void Context::ExplainAddress(u64 addr)
	{
		bool any = false;
		for (const BlockInfo& b : m_blocks)
		{
			if (addr >= b.gpu && addr < b.gpu + b.size)
			{
				any = true;
				Console.Error("deko3d: fault address is in %s (made in batch %llu, %s, offset 0x%llx)", b.tag.c_str(),
					static_cast<unsigned long long>(b.made),
					b.freed ? fmt::format("freed after batch {}", b.freed).c_str() : "still live",
					static_cast<unsigned long long>(addr - b.gpu));
			}
		}
		if (!any)
			Console.Error("deko3d: fault address 0x%llx is in no block the device made", static_cast<unsigned long long>(addr));
		std::fflush(stderr);
	}

	bool Context::Create()
	{
		s_instance = this;
		DkDeviceMaker device_maker;
		dkDeviceMakerDefaults(&device_maker);
		device_maker.cbDebug = DebugCallback;
		// GL's conventions, which PCSX2's OpenGL paths assume: depth 0..1 (clip
		// control) and a lower-left origin, so NDC -1 lands on memory row 0 as
		// in a GL framebuffer object. (Upper-left put it on the last row: render
		// targets came out flipped against copies and readbacks, measured on
		// hardware 2026-10-29.)
		device_maker.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginLowerLeft;
		m_device = dkDeviceCreate(&device_maker);
		if (!m_device)
			return false;

		DkQueueMaker queue_maker;
		dkQueueMakerDefaults(&queue_maker, m_device);
		queue_maker.flags = DkQueueFlags_Graphics | DkQueueFlags_HighPrio | DkQueueFlags_EnableZcull;
		m_queue = dkQueueCreate(&queue_maker);
		if (!m_queue)
			return false;

		for (CmdChunk& chunk : m_cmd_chunks)
		{
			chunk.block = MakeBlock(m_device, CMD_CHUNK_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
			chunk.last_use = 0;
			if (!chunk.block)
				return false;
		}

		DkCmdBufMaker cmd_maker;
		dkCmdBufMakerDefaults(&cmd_maker, m_device);
		cmd_maker.userData = this;
		cmd_maker.cbAddMem = AddCmdMemory;
		m_cmd = dkCmdBufCreate(&cmd_maker);
		if (!m_cmd)
			return false;

		m_stream = MakeBlock(m_device, STREAM_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
		if (!m_stream)
			return false;
		Track(m_stream, "the streaming ring");
		m_stream_cpu = static_cast<u8*>(dkMemBlockGetCpuAddr(m_stream));
		m_stream_gpu = dkMemBlockGetGpuAddr(m_stream);

		const u32 image_bytes = NUM_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor);
		const u32 sampler_bytes = NUM_SAMPLER_DESCRIPTORS * sizeof(DkSamplerDescriptor);
		m_descriptors = MakeBlock(m_device, image_bytes + sampler_bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
		if (!m_descriptors)
			return false;
		Track(m_descriptors, "the descriptor sets");
		u8* desc_cpu = static_cast<u8*>(dkMemBlockGetCpuAddr(m_descriptors));
		std::memset(desc_cpu, 0, image_bytes + sampler_bytes);
		m_image_descs = reinterpret_cast<DkImageDescriptor*>(desc_cpu);
		m_sampler_descs = reinterpret_cast<DkSamplerDescriptor*>(desc_cpu + image_bytes);
		m_image_descs_gpu = dkMemBlockGetGpuAddr(m_descriptors);
		m_sampler_descs_gpu = m_image_descs_gpu + image_bytes;
		m_free_image_slots.reserve(NUM_IMAGE_DESCRIPTORS);
		for (u32 i = NUM_IMAGE_DESCRIPTORS; i-- > 0;)
			m_free_image_slots.push_back(i);

		m_code = MakeBlock(m_device, CODE_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
		if (!m_code)
			return false;
		Track(m_code, "shader code");
		for (u32 i = 0; i < NUM_CMD_CHUNKS; i++)
			Track(m_cmd_chunks[i].block, fmt::format("command chunk {}", i));

		BeginBatch();
		return true;
	}

	void Context::Destroy()
	{
		if (m_queue && !dkQueueIsInErrorState(m_queue))
			dkQueueWaitIdle(m_queue);
		while (!m_deferred.empty())
		{
			m_deferred.front().second();
			m_deferred.pop_front();
		}
		// Anything the GS still holds (ImGui's textures, say) goes too: deko3d
		// will not destroy a device with blocks left.
		for (BlockInfo& b : m_blocks)
		{
			const bool own = b.block == m_stream || b.block == m_descriptors || b.block == m_code ||
							 std::any_of(m_cmd_chunks.begin(), m_cmd_chunks.end(), [&b](const CmdChunk& c) { return c.block == b.block; });
			if (b.freed == 0 && !own)
			{
				Console.WriteLn("deko3d: freeing %s at shutdown", b.tag.c_str());
				dkMemBlockDestroy(b.block);
				b.freed = m_current_batch;
			}
		}
		if (m_cmd)
			dkCmdBufDestroy(m_cmd);
		for (CmdChunk& chunk : m_cmd_chunks)
		{
			if (chunk.block)
				dkMemBlockDestroy(chunk.block);
			chunk.block = nullptr;
		}
		for (DkMemBlock* block : {&m_stream, &m_descriptors, &m_code})
		{
			if (*block)
				dkMemBlockDestroy(*block);
			*block = nullptr;
		}
		if (m_queue)
			dkQueueDestroy(m_queue);
		if (m_device)
			dkDeviceDestroy(m_device);
		m_cmd = nullptr;
		m_queue = nullptr;
		m_device = nullptr;
	}

	void Context::AddCmdMemory(void* user, DkCmdBuf cmd, size_t min_size)
	{
		Context* self = static_cast<Context*>(user);
		if (min_size > CMD_CHUNK_SIZE)
		{
			Console.Error("deko3d: command memory request of %zu bytes is larger than a chunk", min_size);
			return;
		}
		CmdChunk& chunk = self->m_cmd_chunks[self->m_next_cmd_chunk];
		self->m_next_cmd_chunk = (self->m_next_cmd_chunk + 1) % NUM_CMD_CHUNKS;
		if (chunk.last_use == self->m_current_batch)
			Console.Error("deko3d: one batch used all %u command chunks", NUM_CMD_CHUNKS);
		else if (chunk.last_use)
			self->WaitFor(chunk.last_use);
		chunk.last_use = self->m_current_batch;
		dkCmdBufAddMemory(cmd, chunk.block, 0, CMD_CHUNK_SIZE);
	}

	void Context::BindDescriptorSets()
	{
		dkCmdBufBindImageDescriptorSet(m_cmd, m_image_descs_gpu, NUM_IMAGE_DESCRIPTORS);
		dkCmdBufBindSamplerDescriptorSet(m_cmd, m_sampler_descs_gpu, NUM_SAMPLER_DESCRIPTORS);
	}

	void Context::BeginBatch()
	{
		// The chunk being filled carries on into the new batch.
		const u32 active = (m_next_cmd_chunk + NUM_CMD_CHUNKS - 1) % NUM_CMD_CHUNKS;
		if (m_cmd_chunks[active].last_use)
			m_cmd_chunks[active].last_use = m_current_batch;
		BindDescriptorSets();
	}

	bool Context::IsComplete(BatchId id)
	{
		if (id <= m_completed_batch)
			return true;
		if (id >= m_current_batch)
			return false;
		const u32 idx = static_cast<u32>(id % NUM_FENCES);
		if (m_fence_batch[idx] != id || dkFenceWait(&m_fences[idx], 0) == DkResult_Success)
		{
			m_completed_batch = id;
			return true;
		}
		return false;
	}

	void Context::WaitFor(BatchId id)
	{
		if (id <= m_completed_batch)
			return;
		if (id >= m_current_batch)
			Submit();
		const u32 idx = static_cast<u32>(id % NUM_FENCES);
		if (m_fence_batch[idx] == id)
			WaitFence(m_queue, &m_fences[idx], id);
		m_completed_batch = id;
	}

	void Context::Submit()
	{
		const u32 idx = static_cast<u32>(m_current_batch % NUM_FENCES);
		// The fence slot's previous batch must be done before it is reused.
		if (m_fence_batch[idx] && m_fence_batch[idx] > m_completed_batch)
		{
			WaitFence(m_queue, &m_fences[idx], m_fence_batch[idx]);
			m_completed_batch = m_fence_batch[idx];
		}
		// The first batches are traced and flushed to the SD card at once, so a
		// GPU hang still leaves the last step in the log.
		const bool trace = m_current_batch <= 12;
		if (trace)
		{
			Console.WriteLn("deko3d: submitting batch %llu", static_cast<unsigned long long>(m_current_batch));
			std::fflush(stderr);
		}
		dkCmdBufSignalFence(m_cmd, &m_fences[idx], true);
		const DkCmdList list = dkCmdBufFinishList(m_cmd);
		dkQueueSubmitCommands(m_queue, list);
		dkQueueFlush(m_queue);
		if (!m_reported_error && dkQueueIsInErrorState(m_queue))
		{
			m_reported_error = true;
			Console.Error("deko3d: the GPU queue is in an error state after batch %llu", static_cast<unsigned long long>(m_current_batch));
		}
		m_fence_batch[idx] = m_current_batch;
		if (trace)
		{
			const bool done = dkFenceWait(&m_fences[idx], 2'000'000'000) == DkResult_Success;
			Console.WriteLn("deko3d: batch %llu %s", static_cast<unsigned long long>(m_current_batch),
				done ? "finished" : (dkQueueIsInErrorState(m_queue) ? "FAILED (queue error)" : "NOT finished after 2 s"));
			std::fflush(stderr);
		}
		m_stream_marks.emplace_back(m_current_batch, m_stream_head);
		m_current_batch++;
		RetireCompleted();
		BeginBatch();
	}

	bool Context::SubmitAndProbe(BatchId id, s64 timeout_ns)
	{
		Submit();
		const u32 idx = static_cast<u32>(id % NUM_FENCES);
		if (dkFenceWait(&m_fences[idx], timeout_ns) != DkResult_Success)
			return false;
		m_completed_batch = std::max(m_completed_batch, id);
		return true;
	}

	void Context::SubmitAndWait()
	{
		const BatchId id = m_current_batch;
		Submit();
		WaitFor(id);
		RetireCompleted();
	}

	void Context::RetireCompleted()
	{
		while (!m_stream_marks.empty() && IsComplete(m_stream_marks.front().first))
		{
			m_stream_tail = m_stream_marks.front().second;
			m_stream_marks.pop_front();
		}
		while (!m_deferred.empty() && IsComplete(m_deferred.front().first))
		{
			m_deferred.front().second();
			m_deferred.pop_front();
		}
	}

	Context::Allocation Context::Stream(u32 size, u32 align)
	{
		if (size + align > STREAM_SIZE / 2)
			return {nullptr, 0};
		for (;;)
		{
			const bool empty = m_stream_marks.empty() && m_stream_head == m_stream_tail;
			u32 start = AlignUp(m_stream_head, align);
			bool fits;
			if (empty || m_stream_head >= m_stream_tail)
			{
				// Free space is [head, end) and [0, tail).
				fits = start + size <= STREAM_SIZE;
				if (!fits && size < m_stream_tail)
				{
					start = 0;
					fits = true;
				}
				if (empty && !fits)
				{
					start = 0;
					m_stream_tail = 0;
					fits = true;
				}
			}
			else
			{
				fits = start + size < m_stream_tail;
			}
			if (fits)
			{
				m_stream_head = start + size;
				return {m_stream_cpu + start, m_stream_gpu + start};
			}
			// Make room: the oldest batch still holding data has to finish.
			if (m_stream_marks.empty())
				Submit();
			WaitFor(m_stream_marks.front().first);
			RetireCompleted();
			if (m_stream_marks.empty())
			{
				// Everything retired: start over from the beginning.
				m_stream_head = m_stream_tail = 0;
			}
		}
	}

	u32 Context::AllocImageDescriptor()
	{
		if (m_free_image_slots.empty())
		{
			// Freed slots come back as batches complete.
			SubmitAndWait();
			if (m_free_image_slots.empty())
			{
				Console.Error("deko3d: out of image descriptors");
				return 0;
			}
		}
		const u32 slot = m_free_image_slots.back();
		m_free_image_slots.pop_back();
		return slot;
	}

	void Context::FreeImageDescriptor(u32 slot)
	{
		Defer([this, slot]() { m_free_image_slots.push_back(slot); });
	}

	void Context::WriteImageDescriptor(u32 slot, const DkImageView& view)
	{
		dkImageDescriptorInitialize(&m_image_descs[slot], &view, false, false);
		m_descriptors_dirty = true;
	}

	void Context::WriteSamplerDescriptor(u32 slot, const DkSampler& sampler)
	{
		dkSamplerDescriptorInitialize(&m_sampler_descs[slot], &sampler);
		m_descriptors_dirty = true;
	}

	void Context::Defer(std::function<void()> fn)
	{
		m_deferred.emplace_back(m_current_batch, std::move(fn));
	}

	DkMemBlock Context::AllocImageMemory(u32 size, std::string tag)
	{
		DkMemBlock block = MakeBlock(m_device, size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
		Track(block, std::move(tag));
		return block;
	}

	bool Context::AllocCode(u32 size, DkMemBlock* block, u32* offset, void** cpu)
	{
		const u32 start = AlignUp(m_code_used, DK_SHADER_CODE_ALIGNMENT);
		if (start + size + DK_SHADER_CODE_UNUSABLE_SIZE > CODE_SIZE)
			return false;
		m_code_used = start + size;
		*block = m_code;
		*offset = start;
		*cpu = static_cast<u8*>(dkMemBlockGetCpuAddr(m_code)) + start;
		return true;
	}
} // namespace dk
