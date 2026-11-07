// SPDX-License-Identifier: GPL-3.0+
// deko3d with nothing behind it, for the Mac shader harvest (UN5_HOST): the
// Switch's deko3d device (src/deko3d/) runs unchanged, so PCSX2's hardware
// renderer picks the same shaders and the device writes the same sources, but
// no command reaches a GPU. Memory blocks are host memory (what the device
// fills through their CPU address, it can read back); images take a token
// size, since only a GPU would touch them; fences are always signalled.
#include <deko3d.h>

#include <cstdlib>
#include <cstring>
#include <new>

namespace dk::detail {
struct Device
{
	int unused;
};
struct MemBlock
{
	void* cpu;
	uint32_t size;
	bool owned;
};
struct CmdBuf
{
	int unused;
};
struct Queue
{
	int unused;
};
struct Swapchain
{
	int unused;
};
} // namespace dk::detail

namespace {
// What an image layout keeps in its opaque storage here.
struct Layout
{
	uint64_t size;
	uint32_t alignment;
};
constexpr uint64_t kImageSize = 256;
} // namespace

extern "C" {

DkDevice dkDeviceCreate(DkDeviceMaker const*) { return new dk::detail::Device{}; }
void dkDeviceDestroy(DkDevice obj) { delete obj; }

DkMemBlock dkMemBlockCreate(DkMemBlockMaker const* maker)
{
	auto* block = new dk::detail::MemBlock{maker->storage, maker->size, false};
	if (!block->cpu)
	{
		block->cpu = std::aligned_alloc(0x1000, (maker->size + 0xFFF) & ~0xFFFu);
		std::memset(block->cpu, 0, maker->size);
		block->owned = true;
	}
	return block;
}
void dkMemBlockDestroy(DkMemBlock obj)
{
	if (obj->owned)
		std::free(obj->cpu);
	delete obj;
}
void* dkMemBlockGetCpuAddr(DkMemBlock obj) { return obj->cpu; }
DkGpuAddr dkMemBlockGetGpuAddr(DkMemBlock obj) { return reinterpret_cast<DkGpuAddr>(obj->cpu); }
uint32_t dkMemBlockGetSize(DkMemBlock obj) { return obj->size; }

DkResult dkFenceWait(DkFence*, int64_t) { return DkResult_Success; }

DkCmdBuf dkCmdBufCreate(DkCmdBufMaker const*) { return new dk::detail::CmdBuf{}; }
void dkCmdBufDestroy(DkCmdBuf obj) { delete obj; }
void dkCmdBufAddMemory(DkCmdBuf, DkMemBlock, uint32_t, uint32_t) {}
DkCmdList dkCmdBufFinishList(DkCmdBuf) { return 1; }
void dkCmdBufCallList(DkCmdBuf, DkCmdList) {}
void dkCmdBufBarrier(DkCmdBuf, DkBarrier, uint32_t) {}
void dkCmdBufBindShaders(DkCmdBuf, uint32_t, DkShader const* const[], uint32_t) {}
void dkCmdBufBindUniformBuffers(DkCmdBuf, DkStage, uint32_t, DkBufExtents const[], uint32_t) {}
void dkCmdBufBindStorageBuffers(DkCmdBuf, DkStage, uint32_t, DkBufExtents const[], uint32_t) {}
void dkCmdBufBindTextures(DkCmdBuf, DkStage, uint32_t, DkResHandle const[], uint32_t) {}
void dkCmdBufBindImageDescriptorSet(DkCmdBuf, DkGpuAddr, uint32_t) {}
void dkCmdBufBindSamplerDescriptorSet(DkCmdBuf, DkGpuAddr, uint32_t) {}
void dkCmdBufBindRenderTargets(DkCmdBuf, DkImageView const* const[], uint32_t, DkImageView const*) {}
void dkCmdBufBindRasterizerState(DkCmdBuf, DkRasterizerState const*) {}
void dkCmdBufBindMultisampleState(DkCmdBuf, DkMultisampleState const*) {}
void dkCmdBufBindColorState(DkCmdBuf, DkColorState const*) {}
void dkCmdBufBindColorWriteState(DkCmdBuf, DkColorWriteState const*) {}
void dkCmdBufBindBlendStates(DkCmdBuf, uint32_t, DkBlendState const[], uint32_t) {}
void dkCmdBufBindDepthStencilState(DkCmdBuf, DkDepthStencilState const*) {}
void dkCmdBufBindVtxAttribState(DkCmdBuf, DkVtxAttribState const[], uint32_t) {}
void dkCmdBufBindVtxBufferState(DkCmdBuf, DkVtxBufferState const[], uint32_t) {}
void dkCmdBufBindVtxBuffers(DkCmdBuf, uint32_t, DkBufExtents const[], uint32_t) {}
void dkCmdBufBindIdxBuffer(DkCmdBuf, DkIdxFormat, DkGpuAddr) {}
void dkCmdBufSetViewports(DkCmdBuf, uint32_t, DkViewport const[], uint32_t) {}
void dkCmdBufSetScissors(DkCmdBuf, uint32_t, DkScissor const[], uint32_t) {}
void dkCmdBufSetLineWidth(DkCmdBuf, float) {}
void dkCmdBufSetPrimitiveRestart(DkCmdBuf, bool, uint32_t) {}
void dkCmdBufSetStencil(DkCmdBuf, DkFace, uint8_t, uint8_t, uint8_t) {}
void dkCmdBufSetBlendConst(DkCmdBuf, float, float, float, float) {}
void dkCmdBufSignalFence(DkCmdBuf, DkFence*, bool) {}
void dkCmdBufClearColor(DkCmdBuf, uint32_t, uint32_t, const void*) {}
void dkCmdBufClearDepthStencil(DkCmdBuf, bool, float, uint8_t, uint8_t) {}
void dkCmdBufDraw(DkCmdBuf, DkPrimitive, uint32_t, uint32_t, uint32_t, uint32_t) {}
void dkCmdBufDrawIndexed(DkCmdBuf, DkPrimitive, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {}
void dkCmdBufCopyBufferToImage(DkCmdBuf, DkCopyBuf const*, DkImageView const*, DkImageRect const*, uint32_t) {}
void dkCmdBufCopyImage(DkCmdBuf, DkImageView const*, DkImageRect const*, DkImageView const*, DkImageRect const*, uint32_t) {}
void dkCmdBufCopyImageToBuffer(DkCmdBuf, DkImageView const*, DkImageRect const*, DkCopyBuf const*, uint32_t) {}
void dkCmdBufBlitImage(DkCmdBuf, DkImageView const*, DkImageRect const*, DkImageView const*, DkImageRect const*, uint32_t, uint32_t) {}

DkQueue dkQueueCreate(DkQueueMaker const*) { return new dk::detail::Queue{}; }
void dkQueueDestroy(DkQueue obj) { delete obj; }
bool dkQueueIsInErrorState(DkQueue) { return false; }
void dkQueueSubmitCommands(DkQueue, DkCmdList) {}
void dkQueueFlush(DkQueue) {}
void dkQueueWaitIdle(DkQueue) {}
int dkQueueAcquireImage(DkQueue, DkSwapchain) { return 0; }
void dkQueuePresentImage(DkQueue, DkSwapchain, int) {}

DkSwapchain dkSwapchainCreate(DkSwapchainMaker const*) { return new dk::detail::Swapchain{}; }
void dkSwapchainDestroy(DkSwapchain obj) { delete obj; }

void dkShaderInitialize(DkShader* obj, DkShaderMaker const*) { std::memset(obj, 0, sizeof *obj); }

void dkImageLayoutInitialize(DkImageLayout* obj, DkImageLayoutMaker const*)
{
	static_assert(sizeof(Layout) <= sizeof obj->_storage);
	std::memset(obj, 0, sizeof *obj);
	new (obj->_storage) Layout{kImageSize, 0x200};
}
uint64_t dkImageLayoutGetSize(DkImageLayout const* obj) { return reinterpret_cast<const Layout*>(obj->_storage)->size; }
uint32_t dkImageLayoutGetAlignment(DkImageLayout const* obj) { return reinterpret_cast<const Layout*>(obj->_storage)->alignment; }
void dkImageInitialize(DkImage* obj, DkImageLayout const*, DkMemBlock, uint32_t) { std::memset(obj, 0, sizeof *obj); }
void dkImageDescriptorInitialize(DkImageDescriptor* obj, DkImageView const*, bool, bool) { std::memset(obj, 0, sizeof *obj); }
void dkSamplerDescriptorInitialize(DkSamplerDescriptor* obj, DkSampler const*) { std::memset(obj, 0, sizeof *obj); }

}
