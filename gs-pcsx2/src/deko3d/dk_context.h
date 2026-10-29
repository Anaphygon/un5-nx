// SPDX-License-Identifier: GPL-3.0+
// The deko3d side of the GS device: the device and queue, one command buffer
// that is submitted in batches, a fence per batch, a streaming ring for
// everything the CPU writes for the GPU (vertices, indices, uniforms, texture
// uploads), the image and sampler descriptor sets, and deferred destruction
// of what the GPU may still use.
//
// GL conventions are kept on purpose (PCSX2's OpenGL shaders and the GS's
// lower-left-origin paths are reused): depth 0..1, NDC -1 at memory row 0.
#pragma once

#include "common/Pcsx2Types.h"

#include <deko3d.h>

#include <array>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace dk
{
	// Batches are numbered from 1; a batch is complete once its fence signalled.
	using BatchId = u64;

	class Context
	{
	public:
		static constexpr u32 NUM_IMAGE_DESCRIPTORS = 8192;
		static constexpr u32 NUM_SAMPLER_DESCRIPTORS = 512;

		bool Create();
		void Destroy();

		DkDevice Device() const { return m_device; }
		DkQueue Queue() const { return m_queue; }
		DkCmdBuf Cmd() const { return m_cmd; }

		// The batch being recorded now; whatever it uses is safe to reuse once
		// IsComplete() says so.
		BatchId CurrentBatch() const { return m_current_batch; }
		bool IsComplete(BatchId id);
		void WaitFor(BatchId id);

		// Ends the batch being recorded and hands it to the GPU.
		void Submit();
		// Submit, then wait up to timeout_ns for batch id; false if it did not finish.
		bool SubmitAndProbe(BatchId id, s64 timeout_ns);
		// Submit, then wait for everything to finish.
		void SubmitAndWait();

		// Space in the streaming ring, valid for the current batch. Returns
		// nullptr only when the request is larger than the ring.
		struct Allocation
		{
			void* cpu;
			DkGpuAddr gpu;
		};
		Allocation Stream(u32 size, u32 align);

		// Descriptor slots. A freed slot returns to the pool once the batch
		// that last used it is complete.
		u32 AllocImageDescriptor();
		void FreeImageDescriptor(u32 slot);
		void WriteImageDescriptor(u32 slot, const DkImageView& view);
		void WriteSamplerDescriptor(u32 slot, const DkSampler& sampler);

		// Runs fn once the current batch is complete (memory blocks and the
		// like that recorded commands may still read).
		void Defer(std::function<void()> fn);

		// Memory for images: one block per image, rounded to the block alignment.
		// The tag names it in the log when the GPU faults inside it.
		DkMemBlock AllocImageMemory(u32 size, std::string tag);
		// Destroys a block once the current batch is complete.
		void FreeBlock(DkMemBlock block);
		// Every block made, live or freed, by GPU address; logs the ones that
		// contain addr (called from the debug callback on a page fault).
		void Track(DkMemBlock block, std::string tag);
		void ExplainAddress(u64 addr);
		static Context* s_instance;
		// Code memory for shaders, never freed (the pack is small).
		bool AllocCode(u32 size, DkMemBlock* block, u32* offset, void** cpu);

	private:
		static void AddCmdMemory(void* user, DkCmdBuf cmd, size_t min_size);
		void BeginBatch();
		void RetireCompleted();
		void BindDescriptorSets();

		DkDevice m_device = nullptr;
		DkQueue m_queue = nullptr;
		DkCmdBuf m_cmd = nullptr;

		// Command memory: chunks handed to the command buffer as it fills.
		struct CmdChunk
		{
			DkMemBlock block;
			BatchId last_use;
		};
		static constexpr u32 CMD_CHUNK_SIZE = 1024 * 1024;
		static constexpr u32 NUM_CMD_CHUNKS = 48;
		std::array<CmdChunk, NUM_CMD_CHUNKS> m_cmd_chunks{};
		u32 m_next_cmd_chunk = 0;

		// Fences, one per batch in flight.
		static constexpr u32 NUM_FENCES = 64;
		std::array<DkFence, NUM_FENCES> m_fences{};
		std::array<BatchId, NUM_FENCES> m_fence_batch{};
		BatchId m_current_batch = 1;
		BatchId m_completed_batch = 0;
		bool m_reported_error = false;
		struct BlockInfo
		{
			DkMemBlock block;
			u64 gpu;
			u32 size;
			std::string tag;
			BatchId made, freed;
		};
		std::vector<BlockInfo> m_blocks;

		// Streaming ring.
		static constexpr u32 STREAM_SIZE = 96 * 1024 * 1024;
		DkMemBlock m_stream = nullptr;
		u8* m_stream_cpu = nullptr;
		DkGpuAddr m_stream_gpu = 0;
		u32 m_stream_head = 0;
		// Where each batch's writes ended, oldest first.
		std::deque<std::pair<BatchId, u32>> m_stream_marks;
		u32 m_stream_tail = 0; // start of the oldest data still in use

		// Descriptors.
		DkMemBlock m_descriptors = nullptr;
		DkImageDescriptor* m_image_descs = nullptr;
		DkSamplerDescriptor* m_sampler_descs = nullptr;
		DkGpuAddr m_image_descs_gpu = 0, m_sampler_descs_gpu = 0;
		std::vector<u32> m_free_image_slots;
		bool m_descriptors_dirty = false;

		std::deque<std::pair<BatchId, std::function<void()>>> m_deferred;

		// Code memory.
		static constexpr u32 CODE_SIZE = 16 * 1024 * 1024;
		DkMemBlock m_code = nullptr;
		u32 m_code_used = 0;

	public:
		// Set by the device: invalidates descriptor caches before the next draw
		// after a descriptor changed.
		bool TakeDescriptorsDirty()
		{
			const bool d = m_descriptors_dirty;
			m_descriptors_dirty = false;
			return d;
		}
	};
} // namespace dk
