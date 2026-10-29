// SPDX-License-Identifier: GPL-3.0+
#include "deko3d/GSTextureDK.h"
#include "deko3d/GSDeviceDK.h"

#include "GS/GSPerfMon.h"
#include "common/Console.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <cstring>

namespace
{
	constexpr u32 UPLOAD_ALIGNMENT = 64;
	constexpr u32 UPLOAD_PITCH_ALIGNMENT = 64;

	struct FormatInfo
	{
		DkImageFormat format;
		u32 shift;
	};

	FormatInfo GetFormatInfo(GSTexture::Format format)
	{
		switch (format)
		{
			case GSTexture::Format::PrimID: return {DkImageFormat_R32_Float, 2};
			case GSTexture::Format::UInt32: return {DkImageFormat_R32_Uint, 2};
			case GSTexture::Format::UInt16: return {DkImageFormat_R16_Uint, 1};
			case GSTexture::Format::UNorm8: return {DkImageFormat_R8_Unorm, 0};
			case GSTexture::Format::DepthColor: return {DkImageFormat_R32_Float, 2};
			case GSTexture::Format::Color: return {DkImageFormat_RGBA8_Unorm, 2};
			case GSTexture::Format::ColorHQ: return {DkImageFormat_RGB10A2_Unorm, 2};
			case GSTexture::Format::ColorHDR: return {DkImageFormat_RGBA16_Float, 3};
			case GSTexture::Format::ColorClip: return {DkImageFormat_RGBA16_Unorm, 3};
			case GSTexture::Format::DepthStencil: return {DkImageFormat_ZF32_X24S8, 3};
			case GSTexture::Format::BC1: return {DkImageFormat_RGBA_BC1, 1};
			case GSTexture::Format::BC2: return {DkImageFormat_RGBA_BC2, 1};
			case GSTexture::Format::BC3: return {DkImageFormat_RGBA_BC3, 1};
			case GSTexture::Format::BC7: return {DkImageFormat_RGBA_BC7_Unorm, 1};
			default: return {DkImageFormat_None, 0};
		}
	}
} // namespace

GSTextureDK::GSTextureDK(Usage usage, int width, int height, int levels, Format format)
{
	m_size.x = std::max(1, width);
	m_size.y = std::max(1, height);
	m_usage = usage;
	m_format = format;
	m_mipmap_levels = IsTexture() ? std::max(1, levels) : 1;

	const FormatInfo info = GetFormatInfo(format);
	m_dk_format = info.format;
	m_shift = info.shift;
	if (m_dk_format == DkImageFormat_None)
		return;

	GSDeviceDK* const dev = GSDeviceDK::GetInstance();
	dk::Context& ctx = dev->Ctx();

	DkImageLayoutMaker maker;
	dkImageLayoutMakerDefaults(&maker, ctx.Device());
	maker.type = DkImageType_2D;
	maker.format = m_dk_format;
	maker.dimensions[0] = m_size.x;
	maker.dimensions[1] = m_size.y;
	maker.dimensions[2] = 1;
	maker.mipLevels = m_mipmap_levels;
	maker.flags = DkImageFlags_BlockLinear;
	// Every uncompressed image may be drawn to: the GS draws into plain
	// textures too (conversions, the ImGui font's clear), and deko3d requires
	// the flag on any image bound as a target.
	if (!IsCompressedFormat())
		maker.flags |= DkImageFlags_UsageRender;
	if (!IsDepthStencil() && !IsCompressedFormat())
		maker.flags |= DkImageFlags_Usage2DEngine;
	dkImageLayoutInitialize(&m_layout, &maker);

	const u32 size = static_cast<u32>(dkImageLayoutGetSize(&m_layout));
	m_memory = ctx.AllocImageMemory(size, fmt::format("a {}x{} texture (format {}, usage {})", m_size.x, m_size.y,
		static_cast<int>(format), static_cast<int>(usage)));
	if (!m_memory)
	{
		Console.Error("deko3d: failed to allocate %u bytes for a %dx%d texture", size, m_size.x, m_size.y);
		return;
	}
	dkImageInitialize(&m_image, &m_layout, m_memory, 0);

	dkImageViewDefaults(&m_view, &m_image);
	if (format == Format::UNorm8)
	{
		// GL swizzles alpha from red for this format.
		m_view.swizzle[0] = DkImageSwizzle_Red;
		m_view.swizzle[1] = DkImageSwizzle_Green;
		m_view.swizzle[2] = DkImageSwizzle_Blue;
		m_view.swizzle[3] = DkImageSwizzle_Red;
	}
	if (IsDepthStencil())
		m_view.dsSource = DkDsSource_Depth;

	m_descriptor = ctx.AllocImageDescriptor();
	ctx.WriteImageDescriptor(m_descriptor, m_view);
}

GSTextureDK::~GSTextureDK()
{
	if (!m_memory)
		return;
	GSDeviceDK* const dev = GSDeviceDK::GetInstance();
	dev->UnbindTexture(this);
	dk::Context& ctx = dev->Ctx();
	ctx.FreeImageDescriptor(m_descriptor);
	ctx.FreeBlock(m_memory);
}

void* GSTextureDK::GetNativeHandle() const
{
	return const_cast<GSTextureDK*>(this);
}

DkImageView GSTextureDK::LevelView(int level) const
{
	DkImageView view = m_view;
	view.mipLevelOffset = static_cast<u8>(level);
	view.mipLevelCount = 1;
	return view;
}

bool GSTextureDK::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	if (layer >= m_mipmap_levels || !m_memory)
		return true;

	GSDeviceDK* const dev = GSDeviceDK::GetInstance();
	dev->CommitClear(this);

	const u32 row_bytes = IsCompressedFormat() ? CalcUploadPitch(r.width()) : (static_cast<u32>(r.width()) << m_shift);
	const u32 rows = IsCompressedFormat() ? ((r.height() + 3) / 4) : static_cast<u32>(r.height());
	const u32 upload_pitch = Common::AlignUpPow2(row_bytes, UPLOAD_PITCH_ALIGNMENT);
	const dk::Context::Allocation alloc = dev->Ctx().Stream(upload_pitch * rows, UPLOAD_ALIGNMENT);
	if (!alloc.cpu)
	{
		Console.Error("deko3d: texture upload of %ux%u too large", r.width(), r.height());
		return false;
	}
	StringUtil::StrideMemCpy(alloc.cpu, upload_pitch, data, pitch, row_bytes, rows);
	g_perfmon.Put(GSPerfMon::TextureUploads, 1);

	dev->CopyBufferToTexture(this, layer, r, alloc.gpu, upload_pitch);
	m_needs_mipmaps_generated = true;
	return true;
}

bool GSTextureDK::Map(GSMap& m, const GSVector4i* _r, int layer)
{
	if (layer >= m_mipmap_levels || IsCompressedFormat() || !m_memory)
		return false;

	GSDeviceDK* const dev = GSDeviceDK::GetInstance();
	dev->CommitClear(this);

	const GSVector4i r = _r ? *_r : GSVector4i(0, 0, m_size.x, m_size.y);
	const u32 pitch = Common::AlignUpPow2(static_cast<u32>(r.width()) << m_shift, UPLOAD_PITCH_ALIGNMENT);
	const dk::Context::Allocation alloc = dev->Ctx().Stream(pitch * r.height(), UPLOAD_ALIGNMENT);
	if (!alloc.cpu)
		return false;

	g_perfmon.Put(GSPerfMon::TextureUploads, 1);
	m.bits = static_cast<u8*>(alloc.cpu);
	m.pitch = pitch;
	m_map_cpu = static_cast<u8*>(alloc.cpu);
	m_map_gpu = alloc.gpu;
	m_map_rect = r;
	m_map_pitch = pitch;
	m_map_layer = layer;
	return true;
}

void GSTextureDK::Unmap()
{
	if (!m_map_cpu)
		return;
	GSDeviceDK::GetInstance()->CopyBufferToTexture(this, m_map_layer, m_map_rect, m_map_gpu, m_map_pitch);
	m_map_cpu = nullptr;
	m_needs_mipmaps_generated = true;
}

void GSTextureDK::GenerateMipmap()
{
	if (m_mipmap_levels <= 1 || !m_memory)
		return;
	GSDeviceDK::GetInstance()->GenerateMipmaps(this);
	m_needs_mipmaps_generated = false;
}

// ---------------------------------------------------------------------------

GSDownloadTextureDK::GSDownloadTextureDK(u32 width, u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
{
}

GSDownloadTextureDK::~GSDownloadTextureDK()
{
	if (!m_memory)
		return;
	GSDeviceDK::GetInstance()->Ctx().FreeBlock(m_memory);
}

std::unique_ptr<GSDownloadTextureDK> GSDownloadTextureDK::Create(u32 width, u32 height, GSTexture::Format format)
{
	const u32 size = GetBufferSize(width, height, format, UPLOAD_PITCH_ALIGNMENT);
	DkMemBlockMaker maker;
	dkMemBlockMakerDefaults(&maker, GSDeviceDK::GetInstance()->Ctx().Device(),
		(size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1));
	// Uncached on the CPU: what the GPU wrote is read back without cache maintenance.
	maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	DkMemBlock memory = dkMemBlockCreate(&maker);
	if (!memory)
		return {};
	GSDeviceDK::GetInstance()->Ctx().Track(memory, fmt::format("a {}x{} download buffer", width, height));
	std::unique_ptr<GSDownloadTextureDK> ret(new GSDownloadTextureDK(width, height, format));
	ret->m_memory = memory;
	ret->m_cpu = static_cast<u8*>(dkMemBlockGetCpuAddr(memory));
	ret->m_gpu = dkMemBlockGetGpuAddr(memory);
	ret->m_size = size;
	ret->m_map_pointer = ret->m_cpu;
	return ret;
}

void GSDownloadTextureDK::CopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	GSTextureDK* const tex = static_cast<GSTextureDK*>(stex);
	GSDeviceDK* const dev = GSDeviceDK::GetInstance();
	dev->CommitClear(tex);

	u32 copy_offset, copy_size, copy_rows;
	m_current_pitch = GetTransferPitch(use_transfer_pitch ? static_cast<u32>(drc.width()) : m_width, UPLOAD_PITCH_ALIGNMENT);
	GetTransferSize(drc, &copy_offset, &copy_size, &copy_rows);
	g_perfmon.Put(GSPerfMon::Readbacks, 1);

	dev->CopyTextureToBuffer(tex, src_level, src, m_gpu + copy_offset, m_current_pitch);
	m_copy_batch = dev->Ctx().CurrentBatch();
	m_needs_flush = true;
}

bool GSDownloadTextureDK::Map(const GSVector4i& read_rc)
{
	return true;
}

void GSDownloadTextureDK::Unmap()
{
}

void GSDownloadTextureDK::Flush()
{
	if (!m_needs_flush)
		return;
	m_needs_flush = false;
	GSDeviceDK::GetInstance()->Ctx().WaitFor(m_copy_batch);
}
