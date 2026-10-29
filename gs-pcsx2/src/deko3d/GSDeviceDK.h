// SPDX-License-Identifier: GPL-3.0+
// PCSX2's GS device on deko3d, the Switch's native graphics API, in place of
// OpenGL through Mesa. It follows GSDeviceOGL step for step and uses the same
// GLSL: every shader is generated exactly as the OpenGL device would generate
// it and looked up by a hash of that text in a pack of precompiled DKSH files
// (shaders-dk/ on the SD card, built by tools/dk_shaders.py with uam from
// sources the OpenGL device harvested). A shader missing from the pack skips
// its draw and is written to shaders-dk-missing/ for the next pack.
#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "deko3d/GSTextureDK.h"
#include "deko3d/dk_context.h"

#include <array>
#include <string>
#include <unordered_map>

class GSDeviceDK final : public GSDevice
{
public:
	using VSSelector = GSHWDrawConfig::VSSelector;
	using PSSelector = GSHWDrawConfig::PSSelector;
	using PSSamplerSelector = GSHWDrawConfig::SamplerSelector;
	using OMDepthStencilSelector = GSHWDrawConfig::DepthStencilSelector;
	using OMColorMaskSelector = GSHWDrawConfig::ColorMaskSelector;

	enum TextureUnit : u32
	{
		TEXTURE_TEXTURE,
		TEXTURE_PALETTE,
		TEXTURE_RT,
		TEXTURE_PRIMID,
		TEXTURE_DEPTH,
		NUM_TEXTURE_UNITS
	};

	GSDeviceDK();
	~GSDeviceDK() override;

	__fi static GSDeviceDK* GetInstance() { return static_cast<GSDeviceDK*>(g_gs_device.get()); }
	dk::Context& Ctx() { return m_ctx; }

	// Used by GSTextureDK.
	void CommitClear(GSTexture* t);
	void UnbindTexture(GSTextureDK* t);
	void CopyBufferToTexture(GSTextureDK* t, int level, const GSVector4i& r, DkGpuAddr src, u32 pitch);
	void CopyTextureToBuffer(GSTextureDK* t, int level, const GSVector4i& r, DkGpuAddr dst, u32 pitch);
	void GenerateMipmaps(GSTextureDK* t);

	RenderAPI GetRenderAPI() const override;
	bool HasSurface() const override;
	bool Create(GSVSyncMode vsync_mode, bool allow_present_throttle) override;
	void Destroy() override;
	bool UpdateWindow() override;
	void ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale) override;
	bool SupportsExclusiveFullscreen() const override;
	void DestroySurface() override;
	std::string GetDriverInfo() const override;
	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle) override;
	PresentResult BeginPresent(bool frame_skip) override;
	void EndPresent() override;
	bool SetGPUTimingEnabled(bool enabled) override;
	float GetAndResetAccumulatedGPUTime() override;
	bool SetGPUPipelineStatisticsEnabled(bool enabled) override;
	GPUPipelineStatistics GetAndResetAccumulatedGPUPipelineStatistics() override;
	void PushDebugGroup(const char* fmt, ...) override {}
	void PopDebugGroup() override {}
	void InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...) override {}

	std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) override;
	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override;
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, PresentShader shader, float shaderTime, Filter filter) override;
	void UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize) override;
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM) override;
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect) override;
	void DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvertSelector shader) override;
	void RenderHW(GSHWDrawConfig& config) override;
	void ClearSamplerCache() override;

protected:
	using GSDevice::DoStretchRect;
	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvertSelector shader, Filter filter) override;

private:
	// A vertex/fragment pair; either may be missing from the pack.
	struct Program
	{
		const DkShader* vs = nullptr;
		const DkShader* ps = nullptr;
		bool Valid() const { return vs && ps; }
	};

	// Uniforms of the fixed shaders that are loose in PCSX2's GLSL; the pack
	// tool gathers them, in declaration order, into one std140 block here.
	static constexpr u32 LOOSE_UNIFORM_BINDING = 7;

	enum VertexLayout : int
	{
		LAYOUT_GS,    // GSVertex (and GSVertexPT1, which has the same size)
		LAYOUT_IMGUI, // ImDrawVert
		LAYOUT_NONE,  // vertex shader expansion reads storage buffers
	};

	struct BlendState
	{
		bool enable = false;
		DkBlendFactor src = DkBlendFactor_One, dst = DkBlendFactor_Zero;
		DkBlendOp op = DkBlendOp_Add;
		DkBlendFactor src_a = DkBlendFactor_One, dst_a = DkBlendFactor_Zero;
		bool constant_enable = false;
		u8 constant = 0;
	};

	GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format) override;
	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter) override;
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb) override;
	void DoFXAA(GSTexture* sTex, GSTexture* dTex) override;
	void DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4]) override;
	bool DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants) override;

	// Shaders.
	std::string GenGlslHeader(std::string_view entry, DkStage stage, std::string_view macro) const;
	std::string GetShaderSource(std::string_view entry, DkStage stage, std::string_view code, std::string_view macro = {}) const;
	const DkShader* GetShader(DkStage stage, const std::string& source);
	const DkShader* LoadPackedShader(u64 hash);
	void PreloadPack();
	Program GetProgram(const std::string& vs, const std::string& ps);
	std::string GetVSSource(VSSelector sel) const;
	std::string GetPSSource(const PSSelector& sel) const;
	const DkShader* GetTfxVS(VSSelector sel);
	const DkShader* GetTfxPS(const PSSelector& sel);
	bool CreateFixedPrograms();

	// State: setters record what is wanted, FlushState() records the changes
	// into the command buffer before a draw or clear.
	void MarkAllStateDirty();
	void FlushState();
	void BindProgram(const Program& prog);
	void SetLooseUniforms(const void* data, u32 size);
	void PSSetShaderResource(int i, GSTexture* sr);
	void PSSetSampler(u32 sampler_slot);
	void ClearTextureUnits();
	void OMSetRenderTargets(GSTexture* rt, GSTexture* ds_as_rt, GSTexture* ds, const GSVector4i* scissor = nullptr);
	void BindSwapchainTarget();
	void SetViewport(const GSVector2i& size);
	void SetScissor(const GSVector4i& scissor);
	void OMSetColorMask(OMColorMaskSelector sel);
	void OMSetBlend(const BlendState& state);
	void OMSetBlendSimple(bool enable, DkBlendFactor src, DkBlendFactor dst, DkBlendOp op);
	void SetBlendFromConfig(const GSHWDrawConfig::BlendState& blend, OMColorMaskSelector mask);
	void OMSetDepthStencil(u32 key);
	void SetVertexLayout(int layout);
	void SetTopology(DkPrimitive prim) { m_topology = prim; }
	bool IsBoundTarget(const GSTextureDK* t) const;
	bool IsBoundTextureUnit(const GSTextureDK* t) const;

	// Hazards between draws, copies and sampling.
	void Barrier(DkBarrier mode);
	void TextureBarrier();
	void BeforeTransfer(GSTextureDK* src, GSTextureDK* dst);
	void AfterTransfer(GSTextureDK* written);
	void NoteDrawWrites();

	// Geometry and draws.
	void IASetVertexBuffer(const void* vertices, u32 stride, u32 count);
	void IASetIndexBuffer(const void* index, u32 count);
	void VSSetPushConstants(u32 base_vertex, u32 base_index);
	void DrawPrimitive();
	void DrawIndexedPrimitive(int offset, int count);
	void Draw(const GSHWDrawConfig& config, int offset, int count);
	void Draw(const GSHWDrawConfig& config) { Draw(config, 0, static_cast<int>(m_index.count)); }
	void DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds);
	void DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, const GSVector2& ds);
	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		const Program& prog, bool alpha_blend, OMColorMaskSelector cms, Filter filter);
	void SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox);
	GSTexture* InitPrimDateTexture(GSTexture* rt, const GSVector4i& area, SetDATM datm);
	void SendHWDraw(const GSHWDrawConfig& config, bool one_barrier, bool full_barrier);

	void RenderImGui();

	dk::Context m_ctx;

	// Swapchain.
	static constexpr u32 NUM_SWAPCHAIN_IMAGES = 3;
	std::array<DkMemBlock, NUM_SWAPCHAIN_IMAGES> m_swap_memory{};
	std::array<DkImage, NUM_SWAPCHAIN_IMAGES> m_swap_images{};
	DkSwapchain m_swapchain = nullptr;
	int m_swap_slot = 0;
	bool m_rendering_to_swapchain = false;
	dk::BatchId m_frame_batch = 0;

	GSTextureDK* m_dummy = nullptr;

	// Shaders.
	std::unordered_map<u64, DkShader> m_shaders;
	std::unordered_map<u64, bool> m_missing_shaders;
	std::string m_tfx_vgs, m_tfx_fs;
	std::unordered_map<u8, const DkShader*> m_vs_cache;
	struct PSKeyHash
	{
		std::size_t operator()(const PSSelector& p) const noexcept { return std::hash<u64>()(p.key_lo ^ (p.key_hi * 0x9e3779b97f4a7c15ull)); }
	};
	struct PSKeyEq
	{
		bool operator()(const PSSelector& a, const PSSelector& b) const noexcept { return a.key_lo == b.key_lo && a.key_hi == b.key_hi; }
	};
	std::unordered_map<PSSelector, const DkShader*, PSKeyHash, PSKeyEq> m_ps_cache;

	const DkShader* m_convert_vs = nullptr;
	std::vector<Program> m_convert;
	std::array<Program, static_cast<int>(PresentShader::Count)> m_present{};
	std::array<Program, 2> m_merge{};
	std::array<Program, NUM_INTERLACE_SHADERS> m_interlace{};
	std::array<Program, 4> m_date_primid{};
	Program m_shadeboost, m_fxaa, m_imgui;

	// Samplers: slots 0..255 are PCSX2's sampler selectors (0 is point).
	static constexpr u32 SAMPLER_POINT = 0;
	u32 m_sampler_linear = 0;

	// The fixed index buffer for expanding sprites and points.
	DkMemBlock m_expand_ib = nullptr;
	DkGpuAddr m_expand_ib_gpu = 0;

	// Geometry in the stream.
	struct
	{
		DkGpuAddr addr = 0;
		u32 count = 0, stride = 0;
	} m_vertex;
	struct
	{
		DkGpuAddr addr = 0;
		u32 count = 0;
	} m_index;
	DkPrimitive m_topology = DkPrimitive_Triangles;

	// Wanted state, and what changed since it was last recorded.
	const DkShader* m_bound_vs = nullptr;
	const DkShader* m_bound_ps = nullptr;
	std::array<GSTextureDK*, NUM_TEXTURE_UNITS> m_textures{};
	u32 m_sampler = 0;
	GSTextureDK* m_current_rt = nullptr;
	GSTextureDK* m_current_ds_as_rt = nullptr;
	GSTextureDK* m_current_ds = nullptr;
	GSVector2i m_viewport = GSVector2i(1, 1);
	GSVector4i m_scissor = GSVector4i::zero();
	u8 m_color_mask = 0xF;
	BlendState m_blend;
	u32 m_depth_key = 0x100;
	int m_vertex_layout = LAYOUT_GS;
	float m_line_width = 1.0f;
	DkBufExtents m_vs_cb{}, m_ps_cb{}, m_vs_pc{}, m_loose{};
	bool m_shaders_dirty = true, m_textures_dirty = true, m_targets_dirty = true, m_viewport_dirty = true,
		 m_scissor_dirty = true, m_color_mask_dirty = true, m_blend_dirty = true, m_depth_dirty = true,
		 m_layout_dirty = true, m_line_width_dirty = true, m_ubos_dirty = true;
	dk::BatchId m_state_batch = 0;

	// Hazards: the barrier epoch, and copies since the last barrier.
	u64 m_epoch = 1;
	bool m_transfers_since_barrier = false;

	GSHWDrawConfig::VSConstantBuffer m_vs_cb_cache;
	GSHWDrawConfig::PSConstantBuffer m_ps_cb_cache;
};
