// SPDX-License-Identifier: GPL-3.0+
// A GS texture in deko3d memory: one image, one memory block, one slot in the
// image descriptor set. Writes are recorded into the device's command buffer,
// so they stay in order with the draws around them.
#pragma once

#include "GS/Renderers/Common/GSTexture.h"
#include "deko3d/dk_context.h"

#include <memory>

class GSTextureDK final : public GSTexture
{
public:
	GSTextureDK(Usage usage, int width, int height, int levels, Format format);
	~GSTextureDK() override;

	bool IsValid() const { return m_memory != nullptr; }

	void* GetNativeHandle() const override;
	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override;
	bool Map(GSMap& m, const GSVector4i* r = nullptr, int layer = 0) override;
	void Unmap() override;
	void GenerateMipmap() override;
#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override {}
#endif

	const DkImage& Image() const { return m_image; }
	// The whole image, as sampled and as a render target.
	const DkImageView& View() const { return m_view; }
	// One mip level, for copies.
	DkImageView LevelView(int level) const;
	DkImageFormat DkFormat() const { return m_dk_format; }
	u32 Descriptor() const { return m_descriptor; }
	u32 PixelShift() const { return m_shift; }

	bool IsIntegerFormat() const { return m_format == Format::UInt16 || m_format == Format::UInt32; }

	// For hazard tracking: the device's barrier epoch when this image was last
	// written (draw, clear, copy, upload) and last read (sampled, copied from).
	u64 m_last_write = 0;
	u64 m_last_read = 0;
	bool m_needs_mipmaps_generated = false;

private:
	DkImageLayout m_layout{};
	DkImage m_image{};
	DkImageView m_view{};
	DkMemBlock m_memory = nullptr;
	DkImageFormat m_dk_format = DkImageFormat_None;
	u32 m_descriptor = 0;
	u32 m_shift = 0;

	// Pending Map().
	u8* m_map_cpu = nullptr;
	DkGpuAddr m_map_gpu = 0;
	GSVector4i m_map_rect;
	u32 m_map_pitch = 0;
	int m_map_layer = 0;
};

class GSDownloadTextureDK final : public GSDownloadTexture
{
public:
	~GSDownloadTextureDK() override;
	static std::unique_ptr<GSDownloadTextureDK> Create(u32 width, u32 height, GSTexture::Format format);

	void CopyFromTexture(const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch) override;
	bool Map(const GSVector4i& read_rc) override;
	void Unmap() override;
	void Flush() override;
#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override {}
#endif

private:
	GSDownloadTextureDK(u32 width, u32 height, GSTexture::Format format);

	DkMemBlock m_memory = nullptr;
	u8* m_cpu = nullptr;
	DkGpuAddr m_gpu = 0;
	u32 m_size = 0;
	dk::BatchId m_copy_batch = 0;
};
