// SPDX-License-Identifier: GPL-3.0+
// GSAllocateWrappedMemory for the Switch. Elsewhere PCSX2 maps GS memory
// (4 MiB) `repeat` times back to back, so an access running past its end lands
// at its start. Horizon gives homebrew no way to map heap memory twice:
// svcMapProcessMemory on it fails with 0xD401 (seen on a Switch, HOS 23.0.0).
// So the block is plain and `repeat` times as large, zeroed: GS memory proper
// is its first 4 MiB, and a read that runs past the end (a texture wrapping
// around GS memory) sees zeroes instead of the start. Writes are masked to
// 4 MiB by GSLocalMemory. Needs proper testing for games that wrap.
#include <cstdlib>
#include <cstring>

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	void* block = std::aligned_alloc(0x1000, size * repeat);
	if (block)
		std::memset(block, 0, size * repeat);
	return block;
}

void GSFreeWrappedMemory(void* ptr, size_t, size_t)
{
	std::free(ptr);
}
