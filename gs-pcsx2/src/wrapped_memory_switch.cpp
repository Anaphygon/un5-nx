// SPDX-License-Identifier: GPL-3.0+
// GSAllocateWrappedMemory for the Switch. GS memory (4 MiB) is mapped
// `repeat` times back to back, so an access running past its end lands at its
// start, as PCSX2 does with shared memory and mmap elsewhere. Here one block
// of heap is mapped into a reserved region with svcMapProcessMemory.
// Kept apart from PCSX2's headers: libnx's u128 differs from theirs.
// Needs proper testing on hardware.
#include <switch.h>

#include <cstdio>
#include <cstdlib>

namespace {
void* s_backing = nullptr;
}

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	s_backing = std::aligned_alloc(0x1000, size);
	if (!s_backing)
		return nullptr;
	const Handle self = envGetOwnProcessHandle();
	virtmemLock();
	u8* base = static_cast<u8*>(virtmemFindAslr(size * repeat, 0x1000));
	bool ok = base != nullptr;
	for (size_t i = 0; ok && i < repeat; ++i)
	{
		const Result rc = svcMapProcessMemory(base + i * size, self, reinterpret_cast<u64>(s_backing), size);
		if (R_FAILED(rc))
		{
			std::fprintf(stderr, "GSAllocateWrappedMemory: mirror %zu failed (0x%x)\n", i, rc);
			for (size_t j = 0; j < i; ++j)
				svcUnmapProcessMemory(base + j * size, self, reinterpret_cast<u64>(s_backing), size);
			ok = false;
		}
	}
	virtmemUnlock();
	if (!ok)
	{
		std::free(s_backing);
		s_backing = nullptr;
		return nullptr;
	}
	return base;
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	const Handle self = envGetOwnProcessHandle();
	for (size_t i = 0; i < repeat; ++i)
		svcUnmapProcessMemory(static_cast<u8*>(ptr) + i * size, self, reinterpret_cast<u64>(s_backing), size);
	std::free(s_backing);
	s_backing = nullptr;
}
