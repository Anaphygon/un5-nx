// SPDX-License-Identifier: GPL-3.0+
// Follows PCSX2's GSDeviceOGL (pcsx2/GS/Renderers/OpenGL/GSDeviceOGL.cpp,
// GPL-3.0+, PCSX2 Dev Team) function by function; see GSDeviceDK.h.
#include "deko3d/GSDeviceDK.h"
#include "deko3d/dk_shader_hash.h"

#include "GS/GSPerfMon.h"
#include "common/Console.h"
#include "common/ScopedGuard.h"
#include "common/Timer.h"

#include "fmt/format.h"
#include "imgui.h"


#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

// libnx's window functions (switch.h itself clashes with PCSX2's types).
extern "C" {
struct NWindow;
NWindow* nwindowGetDefault(void);
u32 nwindowSetSwapInterval(NWindow* nw, u32 swap_interval);
}

// uam, devkitPro's deko3d shader compiler, built into the NRO as a library
// (third_party_src/uam, every symbol but this one prefixed so it cannot clash
// with Mesa's own copy of the same GLSL compiler).
// Runs it on a thread with a large stack (uam_thread.cpp).
bool un5_uam_compile_big_stack(int stage, const char* glsl, const char* out_path);

// Counters for the port's speed log (defined next to the OpenGL device).
extern double g_un5_render_ms, g_un5_swap_ms, g_un5_program_ms;
extern u64 g_un5_gpu_behind, g_un5_presents;

namespace
{
	constexpr const char* kPackDir = "sdmc:/switch/un5-nx/shaders-dk";
	constexpr const char* kMissingDir = "sdmc:/switch/un5-nx/shaders-dk-missing";

	// Same bindings as the OpenGL device's GLSL.
	constexpr u32 g_vs_pc_index = 4;
	constexpr u32 g_vs_ib_index = 3;
	constexpr u32 g_vs_vb_index = 2;
	constexpr u32 g_vs_cb_index = 1;
	constexpr u32 g_ps_cb_index = 0;

	constexpr u32 kDummyDescriptor = 0;

	constexpr std::array<DkBlendFactor, 16> s_blend_factors = {{
		DkBlendFactor_SrcColor, DkBlendFactor_InvSrcColor, DkBlendFactor_DstColor, DkBlendFactor_InvDstColor,
		DkBlendFactor_Src1Color, DkBlendFactor_InvSrc1Color, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha,
		DkBlendFactor_DstAlpha, DkBlendFactor_InvDstAlpha, DkBlendFactor_Src1Alpha, DkBlendFactor_InvSrc1Alpha,
		DkBlendFactor_ConstColor, DkBlendFactor_InvConstColor, DkBlendFactor_One, DkBlendFactor_Zero,
	}};
	constexpr std::array<DkBlendOp, 3> s_blend_ops = {{DkBlendOp_Add, DkBlendOp_Sub, DkBlendOp_RevSub}};

	// Depth/stencil states of the device's own, past the 32 selector keys.
	constexpr u32 DSS_CONVERT = 0x100;
	constexpr u32 DSS_CONVERT_WRITE = 0x101;
	constexpr u32 DSS_DATE = 0x102;

	struct DkshHeader
	{
		u32 magic, header_sz, control_sz, code_sz, programs_off, num_programs;
	};

	bool ReadFile(const std::string& path, std::vector<u8>* out)
	{
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if (!f)
			return false;
		std::fseek(f, 0, SEEK_END);
		const long size = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		out->resize(size > 0 ? static_cast<size_t>(size) : 0);
		const bool ok = size > 0 && std::fread(out->data(), 1, out->size(), f) == out->size();
		std::fclose(f);
		return ok;
	}

	DkSampler MakeSampler(GSHWDrawConfig::SamplerSelector sel)
	{
		DkSampler s;
		dkSamplerDefaults(&s);
		s.magFilter = sel.IsMagFilterLinear() ? DkFilter_Linear : DkFilter_Nearest;
		s.minFilter = sel.IsMinFilterLinear() ? DkFilter_Linear : DkFilter_Nearest;
		s.mipFilter = !sel.UseMipmapFiltering() ? DkMipFilter_None : (sel.IsMipFilterLinear() ? DkMipFilter_Linear : DkMipFilter_Nearest);
		s.lodClampMin = 0.0f;
		s.lodClampMax = sel.lodclamp ? 0.25f : 1000.0f;
		s.wrapMode[0] = sel.tau ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
		s.wrapMode[1] = sel.tav ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
		s.wrapMode[2] = DkWrapMode_ClampToEdge;
		return s;
	}

	DkImageRect Rect(const GSVector4i& r)
	{
		return DkImageRect{static_cast<u32>(r.x), static_cast<u32>(r.y), 0, static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1};
	}
} // namespace

GSDeviceDK::GSDeviceDK() = default;
GSDeviceDK::~GSDeviceDK() = default;

RenderAPI GSDeviceDK::GetRenderAPI() const
{
	// Not true, but the device keeps OpenGL's conventions (lower-left origin
	// for the display, the same GLSL), which is what this answers for.
	return RenderAPI::OpenGL;
}

bool GSDeviceDK::HasSurface() const { return true; }
bool GSDeviceDK::UpdateWindow() { return true; }
void GSDeviceDK::ResizeWindow(u32, u32, float) {}
bool GSDeviceDK::SupportsExclusiveFullscreen() const { return false; }
void GSDeviceDK::DestroySurface() {}
std::string GSDeviceDK::GetDriverInfo() const { return "deko3d"; }
void GSDeviceDK::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_vsync_mode = mode;
	m_allow_present_throttle = allow_present_throttle;
}
bool GSDeviceDK::SetGPUTimingEnabled(bool) { return false; }
float GSDeviceDK::GetAndResetAccumulatedGPUTime() { return 0.0f; }
bool GSDeviceDK::SetGPUPipelineStatisticsEnabled(bool) { return false; }
GPUPipelineStatistics GSDeviceDK::GetAndResetAccumulatedGPUPipelineStatistics() { return {}; }

bool GSDeviceDK::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	if (!GSDevice::Create(vsync_mode, allow_present_throttle))
		return false;
	if (!AcquireWindow(true))
		return false;
	if (!m_ctx.Create())
	{
		Console.Error("deko3d: failed to create the device");
		return false;
	}
	m_name = "deko3d";
	m_ctx.on_error = [this]() { DumpTransferTrace(); };

	// What GSDeviceOGL::CheckFeatures arrives at on the Switch's Mesa, so the
	// shaders come out the same.
	m_features.broken_point_sampler = false;
	m_features.primitive_id = true;
	m_features.framebuffer_fetch = false;
	m_features.texture_barrier = true;
	m_features.multidraw_fb_copy = false;
	m_features.provoking_vertex_last = true;
	m_features.dxt_textures = false;
	m_features.bptc_textures = false;
	m_features.prefer_new_textures = false;
	m_features.stencil_buffer = true;
	m_features.test_and_sample_depth = false;
	m_features.depth_feedback = GSConfig.DepthFeedbackMode == GSDepthFeedbackMode::Depth;
	m_features.vs_expand = !GSConfig.DisableVertexShaderExpand;
	m_features.point_expand = true;
	m_features.line_expand = false;
	m_features.cas_sharpening = false;
	m_features.aa1 = GSConfig.HWAA1 && m_features.vs_expand && m_features.feedback_loops();
	m_max_texture_size = 16384;

	// The swapchain: three 1280x720 images on the default window.
	{
		DkImageLayoutMaker maker;
		dkImageLayoutMakerDefaults(&maker, m_ctx.Device());
		maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
		maker.format = DkImageFormat_RGBA8_Unorm;
		maker.dimensions[0] = m_window_info.surface_width;
		maker.dimensions[1] = m_window_info.surface_height;
		DkImageLayout layout;
		dkImageLayoutInitialize(&layout, &maker);
		const u32 size = static_cast<u32>(dkImageLayoutGetSize(&layout));
		std::array<const DkImage*, NUM_SWAPCHAIN_IMAGES> images;
		for (u32 i = 0; i < NUM_SWAPCHAIN_IMAGES; i++)
		{
			u32 offset = 0;
			m_swap_memory[i] = m_ctx.AllocImageMemory(size, dkImageLayoutGetAlignment(&layout), fmt::format("swapchain image {}", i), &offset);
			if (!m_swap_memory[i])
				return false;
			dkImageInitialize(&m_swap_images[i], &layout, m_swap_memory[i], offset);
			images[i] = &m_swap_images[i];
		}
		DkSwapchainMaker swap_maker;
		dkSwapchainMakerDefaults(&swap_maker, m_ctx.Device(), nwindowGetDefault(), images.data(), NUM_SWAPCHAIN_IMAGES);
		m_swapchain = dkSwapchainCreate(&swap_maker);
		if (!m_swapchain)
			return false;
		// The runner paces frames itself, as with OpenGL's swap interval 0.
		nwindowSetSwapInterval(nwindowGetDefault(), 0);
	}

	// Descriptor slot 0: a 1x1 texture for units a shader declares but a draw
	// does not use.
	m_dummy = static_cast<GSTextureDK*>(CreateSurface(GSTexture::Usage::Texture, 1, 1, 1, GSTexture::Format::Color));

	for (u32 key = 0; key < 256; key++)
		m_ctx.WriteSamplerDescriptor(key, MakeSampler(PSSamplerSelector(key)));
	PSSamplerSelector bilinear;
	bilinear.biln = true;
	m_sampler_linear = bilinear.key;

	{
		DkMemBlockMaker maker;
		dkMemBlockMakerDefaults(&maker, m_ctx.Device(), (EXPAND_BUFFER_SIZE + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1));
		m_expand_ib = dkMemBlockCreate(&maker);
		if (!m_expand_ib)
			return false;
		m_ctx.Track(m_expand_ib, "the expansion index buffer");
		GenerateExpansionIndexBuffer(dkMemBlockGetCpuAddr(m_expand_ib));
		m_expand_ib_gpu = dkMemBlockGetGpuAddr(m_expand_ib);
	}

	mkdir(kMissingDir, 0777);
	std::optional<std::string> vgs = ReadShaderSource("shaders/opengl/tfx_vgs.glsl");
	std::optional<std::string> fs = ReadShaderSource("shaders/opengl/tfx_fs.glsl");
	if (!vgs.has_value() || !fs.has_value())
	{
		Console.Error("deko3d: failed to read shaders/opengl/tfx_{vgs,fs}.glsl");
		return false;
	}
	m_tfx_vgs = std::move(*vgs);
	m_tfx_fs = std::move(*fs);
	PreloadPack();
	if (!CreateFixedPrograms())
		return false;

	SelfTest();
	std::memset(static_cast<void*>(&m_vs_cb_cache), 0xFF, sizeof(m_vs_cb_cache));
	std::memset(static_cast<void*>(&m_ps_cb_cache), 0xFF, sizeof(m_ps_cb_cache));
	Console.WriteLn("deko3d: device ready, %zu shaders loaded, %zu missing from the pack", m_shaders.size(), m_missing_shaders.size());
	std::fflush(stderr);
	return true;
}

void GSDeviceDK::Destroy()
{
	GSDevice::Destroy();
	if (!m_ctx.Device())
		return;
	if (m_dummy)
	{
		delete m_dummy;
		m_dummy = nullptr;
	}
	m_ctx.SubmitAndWait();
	if (m_swapchain)
		dkSwapchainDestroy(m_swapchain);
	m_swapchain = nullptr;
	for (DkMemBlock& block : m_swap_memory)
	{
		m_ctx.FreeBlock(block);
		block = nullptr;
	}
	m_ctx.FreeBlock(m_expand_ib);
	m_expand_ib = nullptr;
	m_ctx.Destroy();
}

// Steps the GPU through what a first frame does, each in its own batch with
// a bounded wait, and logs which one does not finish. Added after the first
// runs on hardware hung on batch 1 with the queue not in an error state.
void GSDeviceDK::SelfTest()
{
	const auto step = [this](const char* what) {
		const dk::BatchId id = m_ctx.CurrentBatch();
		const bool done = m_ctx.SubmitAndProbe(id, 2'000'000'000);
		Console.WriteLn("deko3d selftest: %s: %s", what, done ? "ok" : "HUNG");
		std::fflush(stderr);
		if (!done)
		{
			// Leave now: presenting from a stuck queue freezes the whole screen.
			Console.Error("deko3d selftest failed; stopping");
			std::fflush(stderr);
			std::abort();
		}
		return done;
	};
	if (!step("empty batch"))
		return;
	CommitClear(m_dummy); // no-op unless cleared; clear it explicitly
	{
		GSTextureDK* const saved = m_current_rt;
		m_current_rt = m_dummy;
		m_targets_dirty = true;
		SetViewport(GSVector2i(1, 1));
		SetScissor(GSVector4i(0, 0, 1, 1));
		FlushState();
		dkCmdBufClearColorFloat(m_ctx.Cmd(), 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
		m_current_rt = saved;
		m_targets_dirty = true;
	}
	if (!step("clear a texture"))
		return;
	const int slot = dkQueueAcquireImage(m_ctx.Queue(), m_swapchain);
	Console.WriteLn("deko3d selftest: acquired swapchain image %d", slot);
	if (!step("acquire a swapchain image"))
		return;
	DkImageView view;
	dkImageViewDefaults(&view, &m_swap_images[slot]);
	const DkImageView* colors[1] = {&view};
	dkCmdBufBindRenderTargets(m_ctx.Cmd(), colors, 1, nullptr);
	const DkScissor sc = {0, 0, m_window_info.surface_width, m_window_info.surface_height};
	dkCmdBufSetScissors(m_ctx.Cmd(), 0, &sc, 1);
	dkCmdBufClearColorFloat(m_ctx.Cmd(), 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
	m_targets_dirty = true;
	m_scissor_dirty = true;
	if (!step("clear the swapchain image"))
		return;
	dkQueuePresentImage(m_ctx.Queue(), m_swapchain, slot);
	step("present");
	OrientationProbe();
}

// Which memory rows a draw at NDC y -1..0 and a scissor at window y 0..1 land
// on, read back from a 4x4 target. OpenGL (what PCSX2's paths assume) puts
// both on the first rows.
void GSDeviceDK::OrientationProbe()
{
	GSTextureDK* const rt = static_cast<GSTextureDK*>(CreateSurface(GSTexture::Usage::RenderTarget, 4, 4, 1, GSTexture::Format::Color));
	std::unique_ptr<GSDownloadTextureDK> dl = GSDownloadTextureDK::Create(4, 4, GSTexture::Format::Color);
	if (!rt || !dl || !m_convert[ShaderConvertSelector(ShaderConvert::COPY).Index()].Valid())
		return;
	const auto clear_target = [this](GSTextureDK* t, float v) {
		OMSetRenderTargets(t, nullptr, nullptr);
		FlushState();
		dkCmdBufClearColorFloat(m_ctx.Cmd(), 0, DkColorMask_RGBA, v, v, v, 1.0f);
		NoteDrawWrites();
	};
	clear_target(m_dummy, 1.0f);
	clear_target(rt, 0.0f);
	rt->SetState(GSTexture::State::Dirty);
	m_dummy->SetState(GSTexture::State::Dirty);
	DoStretchRect(m_dummy, GSVector4(0, 0, 1, 1), rt, GSVector4(0, 0, 4, 2), m_convert[ShaderConvertSelector(ShaderConvert::COPY).Index()],
		false, OMColorMaskSelector(), Nearest);
	OMSetRenderTargets(rt, nullptr, nullptr, nullptr);
	SetScissor(GSVector4i(0, 0, 4, 1));
	FlushState();
	dkCmdBufClearColorFloat(m_ctx.Cmd(), 0, DkColorMask_RGBA, 0.5f, 0.5f, 0.5f, 1.0f);
	NoteDrawWrites();
	dl->CopyFromTexture(GSVector4i(0, 0, 4, 4), rt, GSVector4i(0, 0, 4, 4), 0, true);
	dl->Flush();
	std::string rows;
	for (u32 y = 0; y < 4; y++)
		rows += fmt::format(" row{}={:02x}", y, dl->GetMapPointer()[y * dl->GetMapPitch()]);
	Console.WriteLn("deko3d probe: draw at NDC y -1..0 and scissor at window y 0..1 give%s"
					" (OpenGL: rows 0-1 bright, row 0 grey 7f)", rows.c_str());
	std::fflush(stderr);
	delete rt;
}

GSTexture* GSDeviceDK::CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format)
{
	GSTextureDK* tex = new GSTextureDK(usage, width, height, levels, format);
	if (!tex->IsValid())
	{
		delete tex;
		return nullptr;
	}
	return tex;
}

std::unique_ptr<GSDownloadTexture> GSDeviceDK::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTextureDK::Create(width, height, format);
}

// ---------------------------------------------------------------------------
// Shaders

std::string GSDeviceDK::GenGlslHeader(std::string_view entry, DkStage stage, std::string_view macro) const
{
	// GSDeviceOGL::GenGlslHeader with the answers the Switch's Mesa gives
	// (GL 4.3+, no framebuffer fetch, conservative depth, clip control).
	std::string header = "#version 430 core\n";
	header += "#define HAS_FRAMEBUFFER_FETCH 0\n";
	header += "#define PS_HAS_CONSERVATIVE_DEPTH 1\n";
	if (!m_features.texture_barrier && !m_features.multidraw_fb_copy)
		header += "#define DEPTH_FEEDBACK_SUPPORT 0\n";
	else if (m_features.depth_feedback)
		header += "#define DEPTH_FEEDBACK_SUPPORT 1\n";
	else
		header += "#define DEPTH_FEEDBACK_SUPPORT 2\n";
	header += "#define HAS_CLIP_CONTROL 1\n";
	header += (stage == DkStage_Vertex) ? "#define VERTEX_SHADER 1\n" : "#define FRAGMENT_SHADER 1\n";
	if (entry != "main")
	{
		header += "#define ";
		header += entry;
		header += " main\n";
	}
	header += macro;
	return header;
}

std::string GSDeviceDK::GetShaderSource(std::string_view entry, DkStage stage, std::string_view code, std::string_view macro) const
{
	std::string src = GenGlslHeader(entry, stage, macro);
	src += code;
	return src;
}

const DkShader* GSDeviceDK::GetShader(DkStage stage, const std::string& source)
{
	const u64 hash = un5_shader_hash(source);
	if (const auto it = m_shaders.find(hash); it != m_shaders.end())
		return &it->second;
	if (m_missing_shaders.count(hash))
		return nullptr;

	if (const DkShader* shader = LoadPackedShader(hash))
		return shader;

	// Not in the pack: compile it here, once, into the pack. Sources with
	// uniforms outside a block (only the fixed shaders, which the pack built on
	// the Mac always has) need tools/dk_shaders.py's rewrite and are left out.
	if (!HasLooseUniforms(source))
	{
		Common::Timer timer;
		const std::string path = fmt::format("{}/{:016x}.dksh", kPackDir, hash);
		const bool built = un5_uam_compile_big_stack(static_cast<int>(stage), source.c_str(), path.c_str());
		const double ms = timer.GetTimeMilliseconds();
		g_un5_program_ms += ms;
		if (built)
		{
			if (const DkShader* shader = LoadPackedShader(hash))
			{
				Console.WriteLn("deko3d: compiled shader %016llx on the console in %.0f ms", static_cast<unsigned long long>(hash), ms);
				return shader;
			}
		}
		Console.Warning("deko3d: compiling shader %016llx on the console failed", static_cast<unsigned long long>(hash));
	}

	// Not built: keep the source for the Mac (tools/dk_sync.sh) and skip the draw.
	m_missing_shaders.emplace(hash, true);
	const std::string missing = fmt::format("{}/{:016x}.{}", kMissingDir, hash, stage == DkStage_Vertex ? "vert" : "frag");
	if (std::FILE* f = std::fopen(missing.c_str(), "wb"))
	{
		std::fwrite(source.data(), 1, source.size(), f);
		std::fclose(f);
	}
	Console.Warning("deko3d: shader %016llx is not in the pack", static_cast<unsigned long long>(hash));
	return nullptr;
}

bool GSDeviceDK::HasLooseUniforms(const std::string& source)
{
	size_t pos = 0;
	while ((pos = source.find("uniform ", pos)) != std::string::npos)
	{
		const size_t line_start = source.rfind('\n', pos);
		const size_t start = (line_start == std::string::npos) ? 0 : line_start + 1;
		const size_t end = source.find_first_of(";{\n", pos);
		const std::string_view prefix(source.data() + start, pos - start);
		const bool at_line_start = prefix.find_first_not_of(" \t") == std::string_view::npos;
		const std::string_view decl(source.data() + pos, (end == std::string::npos ? source.size() : end) - pos);
		if (at_line_start && end != std::string::npos && source[end] == ';' && decl.find("sampler") == std::string_view::npos &&
			decl.find("image") == std::string_view::npos)
			return true;
		pos += 8;
	}
	return false;
}

const DkShader* GSDeviceDK::LoadPackedShader(u64 hash)
{
	Common::Timer timer;
	ScopedGuard timed([&timer]() { g_un5_program_ms += timer.GetTimeMilliseconds(); });

	std::vector<u8> file;
	const std::string path = fmt::format("{}/{:016x}.dksh", kPackDir, hash);
	if (!ReadFile(path, &file) || file.size() < sizeof(DkshHeader))
		return nullptr;

	DkshHeader header;
	std::memcpy(&header, file.data(), sizeof(header));
	if (header.control_sz + header.code_sz > file.size())
	{
		Console.Error("deko3d: %s is truncated", path.c_str());
		return nullptr;
	}
	DkMemBlock code_block;
	u32 code_offset;
	void* code_cpu;
	if (!m_ctx.AllocCode(header.code_sz, &code_block, &code_offset, &code_cpu))
	{
		Console.Error("deko3d: out of shader code memory");
		return nullptr;
	}
	std::memcpy(code_cpu, file.data() + header.control_sz, header.code_sz);

	DkShaderMaker maker;
	dkShaderMakerDefaults(&maker, code_block, code_offset);
	maker.control = file.data();
	maker.programId = 0;
	DkShader shader;
	dkShaderInitialize(&shader, &maker);
	return &m_shaders.emplace(hash, shader).first->second;
}

GSDeviceDK::Program GSDeviceDK::GetProgram(const std::string& vs, const std::string& ps)
{
	return Program{GetShader(DkStage_Vertex, vs), GetShader(DkStage_Fragment, ps)};
}

std::string GSDeviceDK::GetVSSource(VSSelector sel) const
{
	std::string macro = fmt::format("#define VS_FST {}\n", static_cast<u32>(sel.fst))
		+ fmt::format("#define VS_IIP {}\n", static_cast<u32>(sel.iip))
		+ fmt::format("#define VS_POINT_SIZE {}\n", static_cast<u32>(sel.point_size))
		+ fmt::format("#define VS_EXPAND {}\n", static_cast<int>(sel.expand));
	std::string src = GenGlslHeader("vs_main", DkStage_Vertex, macro);
	src += m_tfx_vgs;
	return src;
}

std::string GSDeviceDK::GetPSSource(const PSSelector& sel) const
{
	// Exactly GSDeviceOGL::GetPSSource's macros, in its order.
	std::string macro = fmt::format("#define PS_FST {}\n", sel.fst)
		+ fmt::format("#define PS_WMS {}\n", sel.wms)
		+ fmt::format("#define PS_WMT {}\n", sel.wmt)
		+ fmt::format("#define PS_ADJS {}\n", sel.adjs)
		+ fmt::format("#define PS_ADJT {}\n", sel.adjt)
		+ fmt::format("#define PS_AEM_FMT {}\n", sel.aem_fmt)
		+ fmt::format("#define PS_PAL_FMT {}\n", sel.pal_fmt)
		+ fmt::format("#define PS_DST_FMT {}\n", sel.dst_fmt)
		+ fmt::format("#define PS_DEPTH_FMT {}\n", sel.depth_fmt)
		+ fmt::format("#define PS_CHANNEL_FETCH {}\n", sel.channel)
		+ fmt::format("#define PS_URBAN_CHAOS_HLE {}\n", sel.urban_chaos_hle)
		+ fmt::format("#define PS_TALES_OF_ABYSS_HLE {}\n", sel.tales_of_abyss_hle)
		+ fmt::format("#define PS_TEX_IS_FB {}\n", sel.tex_is_fb)
		+ fmt::format("#define PS_AEM {}\n", sel.aem)
		+ fmt::format("#define PS_TFX {}\n", sel.tfx)
		+ fmt::format("#define PS_TCC {}\n", sel.tcc)
		+ fmt::format("#define PS_ATST {}\n", static_cast<u32>(sel.atst))
		+ fmt::format("#define PS_AFAIL {}\n", static_cast<u32>(sel.afail))
		+ fmt::format("#define PS_FOG {}\n", sel.fog)
		+ fmt::format("#define PS_BLEND_HW {}\n", sel.blend_hw)
		+ fmt::format("#define PS_A_MASKED {}\n", sel.a_masked)
		+ fmt::format("#define PS_FBA {}\n", sel.fba)
		+ fmt::format("#define PS_LTF {}\n", sel.ltf)
		+ fmt::format("#define PS_AUTOMATIC_LOD {}\n", sel.automatic_lod)
		+ fmt::format("#define PS_MANUAL_LOD {}\n", sel.manual_lod)
		+ fmt::format("#define PS_COLCLIP {}\n", sel.colclip)
		+ fmt::format("#define PS_DATE {}\n", sel.date)
		+ fmt::format("#define PS_TCOFFSETHACK {}\n", sel.tcoffsethack)
		+ fmt::format("#define PS_REGION_RECT {}\n", sel.region_rect)
		+ fmt::format("#define PS_BLEND_A {}\n", sel.blend_a)
		+ fmt::format("#define PS_BLEND_B {}\n", sel.blend_b)
		+ fmt::format("#define PS_BLEND_C {}\n", sel.blend_c)
		+ fmt::format("#define PS_BLEND_D {}\n", sel.blend_d)
		+ fmt::format("#define PS_IIP {}\n", sel.iip)
		+ fmt::format("#define PS_SHUFFLE {}\n", sel.shuffle)
		+ fmt::format("#define PS_SHUFFLE_SAME {}\n", sel.shuffle_same)
		+ fmt::format("#define PS_PROCESS_BA {}\n", sel.process_ba)
		+ fmt::format("#define PS_PROCESS_RG {}\n", sel.process_rg)
		+ fmt::format("#define PS_SHUFFLE_ACROSS {}\n", sel.shuffle_across)
		+ fmt::format("#define PS_READ16_SRC {}\n", sel.real16src)
		+ fmt::format("#define PS_WRITE_RG {}\n", sel.write_rg)
		+ fmt::format("#define PS_FBMASK {}\n", sel.fbmask)
		+ fmt::format("#define PS_COLCLIP_HW {}\n", sel.colclip_hw)
		+ fmt::format("#define PS_RTA_CORRECTION {}\n", sel.rta_correction)
		+ fmt::format("#define PS_RTA_SRC_CORRECTION {}\n", sel.rta_source_correction)
		+ fmt::format("#define PS_DITHER {}\n", sel.dither)
		+ fmt::format("#define PS_DITHER_ADJUST {}\n", sel.dither_adjust)
		+ fmt::format("#define PS_ZCLAMP {}\n", sel.zclamp)
		+ fmt::format("#define PS_ZFLOOR {}\n", sel.zfloor)
		+ fmt::format("#define PS_BLEND_MIX {}\n", sel.blend_mix)
		+ fmt::format("#define PS_ROUND_INV {}\n", sel.round_inv)
		+ fmt::format("#define PS_FIXED_ONE_A {}\n", sel.fixed_one_a)
		+ fmt::format("#define PS_PABE {}\n", sel.pabe)
		+ fmt::format("#define PS_SCANMSK {}\n", sel.scanmsk)
		+ fmt::format("#define PS_NO_COLOR {}\n", sel.no_color)
		+ fmt::format("#define PS_NO_COLOR1 {}\n", sel.no_color1)
		+ fmt::format("#define PS_ZTST {}\n", sel.ztst)
		+ fmt::format("#define PS_AA1 {}\n", static_cast<u32>(sel.aa1))
		+ fmt::format("#define PS_ABE {}\n", sel.abe)
		+ fmt::format("#define PS_ANISOTROPIC_FILTERING {}\n", sel.sw_aniso)
		+ fmt::format("#define PS_ROV_COLOR {}\n", 0)
		+ fmt::format("#define PS_ROV_DEPTH {}\n", 0);
	std::string src = GenGlslHeader("ps_main", DkStage_Fragment, macro);
	src += m_tfx_fs;
	return src;
}

// Loads the whole pack up front, as an emulator builds its shader cache at
// start, so nothing is read or set up in the middle of a fight.
void GSDeviceDK::PreloadPack()
{
	Common::Timer timer;
	DIR* dir = opendir(kPackDir);
	if (!dir)
	{
		Console.Warning("deko3d: no shader pack in %s", kPackDir);
		return;
	}
	while (const dirent* entry = readdir(dir))
	{
		unsigned long long hash = 0;
		char ext[8] = {};
		if (std::sscanf(entry->d_name, "%16llx.%7s", &hash, ext) == 2 && std::strcmp(ext, "dksh") == 0 && !m_shaders.count(hash))
			LoadPackedShader(hash);
	}
	closedir(dir);
	Console.WriteLn("deko3d: %zu shaders from the pack in %.0f ms", m_shaders.size(), timer.GetTimeMilliseconds());
}

bool GSDeviceDK::CreateFixedPrograms()
{
	const std::optional<std::string> convert_glsl = ReadShaderSource("shaders/opengl/convert.glsl");
	const std::optional<std::string> present_glsl = ReadShaderSource("shaders/opengl/present.glsl");
	const std::optional<std::string> merge_glsl = ReadShaderSource("shaders/opengl/merge.glsl");
	const std::optional<std::string> interlace_glsl = ReadShaderSource("shaders/opengl/interlace.glsl");
	const std::optional<std::string> shadeboost_glsl = ReadShaderSource("shaders/opengl/shadeboost.glsl");
	const std::optional<std::string> imgui_glsl = ReadShaderSource("shaders/opengl/imgui.glsl");
	if (!convert_glsl || !present_glsl || !merge_glsl || !interlace_glsl || !shadeboost_glsl || !imgui_glsl)
	{
		Console.Error("deko3d: failed to read the fixed shaders");
		return false;
	}

	const std::string convert_vs = GetShaderSource("vs_main", DkStage_Vertex, *convert_glsl);
	m_convert_vs = GetShader(DkStage_Vertex, convert_vs);
	m_convert.resize(ShaderConvertSelector::NUM_TOTAL_SHADERS);
	for (u32 i = 0; i < ShaderConvertSelector::NUM_TOTAL_SHADERS; i++)
	{
		const ShaderConvertSelector shader = ShaderConvertSelector::Get(i);
		std::string macro;
		macro += fmt::format("#define PRIMID_MAX {}\n", GSShader::PRIMID_MAX);
		macro += fmt::format("#define PRIMID_MIN {}\n", GSShader::PRIMID_MIN);
		macro += fmt::format("#define HAS_BILN {}\n", static_cast<int>(shader.Biln()));
		macro += fmt::format("#define HAS_STENCIL_OUTPUT {}\n", static_cast<int>(shader.StencilOutput()));
		macro += fmt::format("#define HAS_INTEGER_OUTPUT {}\n", static_cast<int>(shader.IntegerOutputBpp() != 0));
		macro += fmt::format("#define HAS_DEPTH_OUTPUT {}\n", static_cast<int>(shader.DepthOutput()));
		macro += fmt::format("#define HAS_FLOAT32_INPUT {}\n", static_cast<int>(shader.Float32Input()));
		macro += fmt::format("#define HAS_FLOAT32_OUTPUT {}\n", static_cast<int>(shader.Float32Output()));
		m_convert[i] = Program{m_convert_vs, GetShader(DkStage_Fragment, GetShaderSource(shader.EntryPoint(), DkStage_Fragment, *convert_glsl, macro))};
	}

	const std::string present_vs = GetShaderSource("vs_main", DkStage_Vertex, *present_glsl);
	for (size_t i = 0; i < m_present.size(); i++)
	{
		const char* name = ShaderEntryPoint(static_cast<PresentShader>(i));
		m_present[i] = GetProgram(present_vs, GetShaderSource(name, DkStage_Fragment, *present_glsl));
	}
	for (size_t i = 0; i < m_merge.size(); i++)
		m_merge[i] = Program{m_convert_vs, GetShader(DkStage_Fragment, GetShaderSource(fmt::format("ps_main{}", i), DkStage_Fragment, *merge_glsl))};
	for (size_t i = 0; i < m_interlace.size(); i++)
		m_interlace[i] = Program{m_convert_vs, GetShader(DkStage_Fragment, GetShaderSource(fmt::format("ps_main{}", i), DkStage_Fragment, *interlace_glsl))};
	m_shadeboost = Program{m_convert_vs, GetShader(DkStage_Fragment, GetShaderSource("ps_main", DkStage_Fragment, *shadeboost_glsl))};
	if (const std::optional<std::string> fxaa = ReadShaderSource("shaders/common/fxaa.fx"))
		m_fxaa = Program{m_convert_vs, GetShader(DkStage_Fragment, GetShaderSource("main", DkStage_Fragment, *fxaa, "#define FXAA_GLSL_130 1\n"))};
	for (size_t i = 0; i < m_date_primid.size(); i++)
	{
		std::string macro;
		macro += fmt::format("#define PRIMID_MAX {}\n", GSShader::PRIMID_MAX);
		macro += fmt::format("#define PRIMID_MIN {}\n", GSShader::PRIMID_MIN);
		m_date_primid[i] = Program{m_convert_vs,
			GetShader(DkStage_Fragment, GetShaderSource(fmt::format("ps_primid_image_init_{}", i), DkStage_Fragment, *convert_glsl, macro))};
	}
	m_imgui = GetProgram(GetShaderSource("vs_main", DkStage_Vertex, *imgui_glsl), GetShaderSource("ps_main", DkStage_Fragment, *imgui_glsl));
	return true;
}

// ---------------------------------------------------------------------------
// State

void GSDeviceDK::MarkAllStateDirty()
{
	m_shaders_dirty = true;
	m_textures_dirty = true;
	m_targets_dirty = true;
	m_viewport_dirty = true;
	m_scissor_dirty = true;
	m_color_mask_dirty = true;
	m_blend_dirty = true;
	m_depth_dirty = true;
	m_layout_dirty = true;
	m_line_width_dirty = true;
	m_ubos_dirty = true;
}

void GSDeviceDK::BindProgram(const Program& prog)
{
	if (m_bound_vs != prog.vs || m_bound_ps != prog.ps)
	{
		m_bound_vs = prog.vs;
		m_bound_ps = prog.ps;
		m_shaders_dirty = true;
	}
}

void GSDeviceDK::SetLooseUniforms(const void* data, u32 size)
{
	const dk::Context::Allocation alloc = m_ctx.Stream(size, DK_UNIFORM_BUF_ALIGNMENT);
	std::memcpy(alloc.cpu, data, size);
	m_loose = DkBufExtents{alloc.gpu, (size + 15u) & ~15u};
	m_ubos_dirty = true;
}

void GSDeviceDK::PSSetShaderResource(int i, GSTexture* sr)
{
	GSTextureDK* const tex = static_cast<GSTextureDK*>(sr);
	if (m_textures[i] != tex)
	{
		m_textures[i] = tex;
		m_textures_dirty = true;
	}
}

void GSDeviceDK::PSSetSampler(u32 sampler_slot)
{
	if (m_sampler != sampler_slot)
	{
		m_sampler = sampler_slot;
		m_textures_dirty = true;
	}
}

void GSDeviceDK::ClearTextureUnits()
{
	for (GSTextureDK*& t : m_textures)
	{
		if (t)
		{
			t = nullptr;
			m_textures_dirty = true;
		}
	}
}

void GSDeviceDK::UnbindTexture(GSTextureDK* t)
{
	for (GSTextureDK*& bound : m_textures)
	{
		if (bound == t)
		{
			bound = nullptr;
			m_textures_dirty = true;
		}
	}
	for (GSTextureDK** target : {&m_current_rt, &m_current_ds_as_rt, &m_current_ds})
	{
		if (*target == t)
		{
			*target = nullptr;
			m_targets_dirty = true;
		}
	}
}

void GSDeviceDK::SetViewport(const GSVector2i& size)
{
	if (m_viewport != size)
	{
		m_viewport = size;
		m_viewport_dirty = true;
	}
}

void GSDeviceDK::SetScissor(const GSVector4i& scissor)
{
	if (!m_scissor.eq(scissor))
	{
		m_scissor = scissor;
		m_scissor_dirty = true;
	}
}

void GSDeviceDK::OMSetColorMask(OMColorMaskSelector sel)
{
	if (m_color_mask != sel.wrgba)
	{
		m_color_mask = sel.wrgba;
		m_color_mask_dirty = true;
	}
}

void GSDeviceDK::OMSetBlend(const BlendState& state)
{
	if (std::memcmp(&m_blend, &state, sizeof(state)) != 0)
	{
		m_blend = state;
		m_blend_dirty = true;
	}
}

void GSDeviceDK::OMSetBlendSimple(bool enable, DkBlendFactor src, DkBlendFactor dst, DkBlendOp op)
{
	BlendState s;
	s.enable = enable;
	if (enable)
	{
		s.src = src;
		s.dst = dst;
		s.op = op;
	}
	OMSetBlend(s);
}

void GSDeviceDK::SetBlendFromConfig(const GSHWDrawConfig::BlendState& blend, OMColorMaskSelector mask)
{
	if (!blend.IsEffective(mask))
	{
		OMSetBlend(BlendState());
		return;
	}
	BlendState s;
	s.enable = blend.enable;
	s.src = s_blend_factors[blend.src_factor];
	s.dst = s_blend_factors[blend.dst_factor];
	s.op = s_blend_ops[blend.op];
	s.src_a = s_blend_factors[blend.src_factor_alpha];
	s.dst_a = s_blend_factors[blend.dst_factor_alpha];
	s.constant_enable = blend.constant_enable;
	s.constant = blend.constant;
	OMSetBlend(s);
}

void GSDeviceDK::OMSetDepthStencil(u32 key)
{
	if (m_depth_key != key)
	{
		m_depth_key = key;
		m_depth_dirty = true;
	}
}

void GSDeviceDK::SetVertexLayout(int layout)
{
	if (m_vertex_layout != layout)
	{
		m_vertex_layout = layout;
		m_layout_dirty = true;
	}
}

void GSDeviceDK::OMSetRenderTargets(GSTexture* rt, GSTexture* ds_as_rt, GSTexture* ds, const GSVector4i* scissor)
{
	GSTextureDK* const RT = static_cast<GSTextureDK*>(rt);
	GSTextureDK* const DSRT = static_cast<GSTextureDK*>(ds_as_rt);
	GSTextureDK* const DS = static_cast<GSTextureDK*>(ds);
	if (RT)
		CommitClear(RT);
	if (DSRT)
		CommitClear(DSRT);
	if (DS)
	{
		CommitClear(DS);
		if (!DSRT)
			InvalidateDSAsRT(DS);
	}
	const bool changed = m_rendering_to_swapchain || RT != m_current_rt || DSRT != m_current_ds_as_rt || DS != m_current_ds;
	g_perfmon.Put(GSPerfMon::RenderPasses, static_cast<double>(changed));
	if (changed)
	{
		m_rendering_to_swapchain = false;
		m_current_rt = RT;
		m_current_ds_as_rt = DSRT;
		m_current_ds = DS;
		m_targets_dirty = true;
	}
	if (RT || DSRT || DS)
	{
		const GSVector2i size = RT ? RT->GetSize() : (DSRT ? DSRT->GetSize() : DS->GetSize());
		SetViewport(size);
		SetScissor(scissor ? *scissor : GSVector4i::loadh(size));
	}
}

void GSDeviceDK::BindSwapchainTarget()
{
	m_rendering_to_swapchain = true;
	m_current_rt = m_current_ds_as_rt = m_current_ds = nullptr;
	m_targets_dirty = true;
	SetViewport(GetWindowSize());
	SetScissor(GSVector4i::loadh(GetWindowSize()));
}

// ---------------------------------------------------------------------------
// Hazards
//
// The GPU runs commands in order but overlaps them and caches textures, so a
// write followed by a read of the same image (or a read followed by a write)
// needs a barrier in between. Each image remembers the epoch of its last write
// and read; a barrier starts a new epoch.

void GSDeviceDK::Barrier(DkBarrier mode)
{
	dkCmdBufBarrier(m_ctx.Cmd(), mode, DkInvalidateFlags_Image);
	m_epoch++;
	m_transfers_since_barrier = false;
	g_perfmon.Put(GSPerfMon::Barriers, 1);
}

void GSDeviceDK::TextureBarrier()
{
	Barrier(DkBarrier_Fragments);
}

void GSDeviceDK::BeforeTransfer(GSTextureDK* src, GSTextureDK* dst)
{
	const bool hazard = (src && src->m_last_write == m_epoch) || (dst && (dst->m_last_write == m_epoch || dst->m_last_read == m_epoch));
	if (hazard)
		Barrier(DkBarrier_Full);
	if (src)
		src->m_last_read = m_epoch;
}

void GSDeviceDK::AfterTransfer(GSTextureDK* written)
{
	if (written)
		written->m_last_write = m_epoch;
	m_transfers_since_barrier = true;
}

void GSDeviceDK::NoteDrawWrites()
{
	if (m_current_rt)
		m_current_rt->m_last_write = m_epoch;
	if (m_current_ds_as_rt)
		m_current_ds_as_rt->m_last_write = m_epoch;
	if (m_current_ds)
		m_current_ds->m_last_write = m_epoch;
}

bool GSDeviceDK::IsBoundTarget(const GSTextureDK* t) const
{
	return t == m_current_rt || t == m_current_ds_as_rt || t == m_current_ds;
}

void GSDeviceDK::FlushState()
{
	const DkCmdBuf cmd = m_ctx.Cmd();
	if (m_state_batch != m_ctx.CurrentBatch())
	{
		// A new command list: record the whole state again.
		m_state_batch = m_ctx.CurrentBatch();
		MarkAllStateDirty();
		DkRasterizerState rs;
		dkRasterizerStateDefaults(&rs);
		rs.cullMode = DkFace_None;
		rs.provokingVertex = DkProvokingVertex_Last;
		dkCmdBufBindRasterizerState(cmd, &rs);
		DkMultisampleState ms;
		dkMultisampleStateDefaults(&ms);
		dkCmdBufBindMultisampleState(cmd, &ms);
		// glStencilFunc(func, 1, 1) and glStencilMask(0xFF).
		dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, 0xFF, 1, 1);
		dkCmdBufSetPrimitiveRestart(cmd, false, 0);
	}

	if (m_ctx.TakeDescriptorsDirty())
		dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);

	// Sampling an image written since the last barrier, or rendering to one
	// read or copied into since then, needs a barrier first. Images that are
	// both bound and rendered to are feedback loops; PCSX2 asks for those
	// barriers itself (TextureBarrier).
	bool hazard = false;
	for (GSTextureDK* t : m_textures)
	{
		if (t && t->m_last_write == m_epoch && !IsBoundTarget(t))
			hazard = true;
	}
	for (GSTextureDK* t : {m_current_rt, m_current_ds_as_rt, m_current_ds})
	{
		if (t && ((m_transfers_since_barrier && t->m_last_write == m_epoch) || (t->m_last_read == m_epoch && !IsBoundTextureUnit(t))))
			hazard = true;
	}
	if (hazard)
		Barrier(DkBarrier_Fragments);
	for (GSTextureDK* t : m_textures)
	{
		if (t)
			t->m_last_read = m_epoch;
	}

	if (m_targets_dirty)
	{
		m_targets_dirty = false;
		m_color_mask_dirty = true;
		DkImageView colors[2];
		const DkImageView* color_ptrs[2] = {nullptr, nullptr};
		u32 num_colors = 0;
		const DkImageView* depth_ptr = nullptr;
		DkImageView depth;
		if (m_rendering_to_swapchain)
		{
			dkImageViewDefaults(&colors[0], &m_swap_images[m_swap_slot]);
			color_ptrs[0] = &colors[0];
			num_colors = 1;
		}
		else
		{
			if (m_current_rt)
			{
				colors[0] = m_current_rt->View();
				color_ptrs[0] = &colors[0];
				num_colors = 1;
			}
			if (m_current_ds_as_rt)
			{
				colors[1] = m_current_ds_as_rt->View();
				color_ptrs[1] = &colors[1];
				num_colors = 2;
			}
			if (m_current_ds)
			{
				dkImageViewDefaults(&depth, &m_current_ds->Image());
				depth_ptr = &depth;
			}
		}
		dkCmdBufBindRenderTargets(cmd, color_ptrs, num_colors, depth_ptr);
	}
	if (m_viewport_dirty)
	{
		m_viewport_dirty = false;
		const DkViewport vp = {0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y), 0.0f, 1.0f};
		dkCmdBufSetViewports(cmd, 0, &vp, 1);
	}
	if (m_scissor_dirty)
	{
		m_scissor_dirty = false;
		const GSVector4i s = m_scissor.max_i32(GSVector4i::zero());
		const DkScissor sc = {static_cast<u32>(s.x), static_cast<u32>(s.y), static_cast<u32>(std::max(0, s.z - s.x)), static_cast<u32>(std::max(0, s.w - s.y))};
		dkCmdBufSetScissors(cmd, 0, &sc, 1);
	}
	if (m_shaders_dirty && m_bound_vs && m_bound_ps)
	{
		m_shaders_dirty = false;
		const DkShader* shaders[2] = {m_bound_vs, m_bound_ps};
		dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
	}
	if (m_textures_dirty)
	{
		m_textures_dirty = false;
		DkResHandle handles[NUM_TEXTURE_UNITS];
		for (u32 i = 0; i < NUM_TEXTURE_UNITS; i++)
		{
			const u32 image = m_textures[i] ? m_textures[i]->Descriptor() : kDummyDescriptor;
			const u32 sampler = (i == TEXTURE_TEXTURE) ? m_sampler : SAMPLER_POINT;
			handles[i] = dkMakeTextureHandle(image, sampler);
		}
		dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, handles, NUM_TEXTURE_UNITS);
	}
	if (m_color_mask_dirty || m_blend_dirty)
	{
		m_color_mask_dirty = false;
		DkColorWriteState cws;
		dkColorWriteStateDefaults(&cws);
		// The swapchain image is composited with its alpha; the PS2's alpha
		// (often 0.5) would show the frame see-through, so it stays at 1.
		dkColorWriteStateSetMask(&cws, 0, m_color_mask & (m_rendering_to_swapchain ? 0x7 : 0xF));
		dkCmdBufBindColorWriteState(cmd, &cws);
	}
	if (m_blend_dirty)
	{
		m_blend_dirty = false;
		// GL_BLEND applies to every draw buffer, the depth-as-colour one too.
		DkColorState cs;
		dkColorStateDefaults(&cs);
		cs.blendEnableMask = m_blend.enable ? 0x3 : 0x0;
		dkCmdBufBindColorState(cmd, &cs);
		if (m_blend.enable)
		{
			DkBlendState bs[2];
			for (DkBlendState& b : bs)
			{
				dkBlendStateDefaults(&b);
				dkBlendStateSetOps(&b, m_blend.op, DkBlendOp_Add);
				dkBlendStateSetFactors(&b, m_blend.src, m_blend.dst, m_blend.src_a, m_blend.dst_a);
			}
			dkCmdBufBindBlendStates(cmd, 0, bs, 2);
			if (m_blend.constant_enable)
			{
				const float bf = static_cast<float>(m_blend.constant) / 128.0f;
				dkCmdBufSetBlendConst(cmd, bf, bf, bf, bf);
			}
		}
	}
	if (m_depth_dirty)
	{
		m_depth_dirty = false;
		DkDepthStencilState ds;
		dkDepthStencilStateDefaults(&ds);
		ds.depthTestEnable = false;
		ds.depthWriteEnable = false;
		ds.stencilTestEnable = false;
		ds.depthCompareOp = DkCompareOp_Always;
		ds.stencilFrontFailOp = ds.stencilBackFailOp = DkStencilOp_Keep;
		ds.stencilFrontDepthFailOp = ds.stencilBackDepthFailOp = DkStencilOp_Keep;
		if (m_depth_key == DSS_CONVERT_WRITE)
		{
			ds.depthTestEnable = true;
			ds.depthWriteEnable = true;
		}
		else if (m_depth_key == DSS_DATE)
		{
			ds.stencilTestEnable = true;
			ds.stencilFrontCompareOp = ds.stencilBackCompareOp = DkCompareOp_Always;
			ds.stencilFrontPassOp = ds.stencilBackPassOp = DkStencilOp_Replace;
		}
		else if (m_depth_key < DSS_CONVERT)
		{
			const OMDepthStencilSelector sel(static_cast<u8>(m_depth_key));
			if (sel.date)
			{
				ds.stencilTestEnable = true;
				ds.stencilFrontCompareOp = ds.stencilBackCompareOp = DkCompareOp_Equal;
				ds.stencilFrontPassOp = ds.stencilBackPassOp = sel.date_one ? DkStencilOp_Zero : DkStencilOp_Keep;
			}
			if (sel.ztst != ZTST_ALWAYS || sel.zwe)
			{
				static constexpr DkCompareOp ztst[] = {DkCompareOp_Never, DkCompareOp_Always, DkCompareOp_Gequal, DkCompareOp_Greater};
				ds.depthTestEnable = true;
				ds.depthCompareOp = ztst[sel.ztst];
				ds.depthWriteEnable = sel.zwe;
			}
		}
		dkCmdBufBindDepthStencilState(cmd, &ds);
	}
	if (m_layout_dirty)
	{
		m_layout_dirty = false;
		static const DkVtxAttribState gs_vertex[] = {
			{0, 0, 0, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
			{0, 0, 16, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
			{0, 0, 8, DkVtxAttribSize_4x8, DkVtxAttribType_Uint, 0},
			{0, 0, 12, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
			{0, 0, 16, DkVtxAttribSize_2x16, DkVtxAttribType_Uint, 0},
			{0, 0, 20, DkVtxAttribSize_1x32, DkVtxAttribType_Uint, 0},
			{0, 0, 24, DkVtxAttribSize_2x16, DkVtxAttribType_Uint, 0},
			{0, 0, 28, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
		};
		static const DkVtxAttribState imgui_vertex[] = {
			{0, 0, offsetof(ImDrawVert, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
			{0, 0, offsetof(ImDrawVert, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
			{0, 0, offsetof(ImDrawVert, col), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
		};
		if (m_vertex_layout == LAYOUT_GS)
		{
			const DkVtxBufferState vb = {sizeof(GSVertex), 0};
			dkCmdBufBindVtxAttribState(cmd, gs_vertex, std::size(gs_vertex));
			dkCmdBufBindVtxBufferState(cmd, &vb, 1);
		}
		else if (m_vertex_layout == LAYOUT_IMGUI)
		{
			const DkVtxBufferState vb = {sizeof(ImDrawVert), 0};
			dkCmdBufBindVtxAttribState(cmd, imgui_vertex, std::size(imgui_vertex));
			dkCmdBufBindVtxBufferState(cmd, &vb, 1);
		}
		else
		{
			dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
			dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
		}
	}
	if (m_line_width_dirty)
	{
		m_line_width_dirty = false;
		dkCmdBufSetLineWidth(cmd, m_line_width);
	}
	if (m_ubos_dirty)
	{
		m_ubos_dirty = false;
		if (m_vs_cb.addr)
			dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, g_vs_cb_index, m_vs_cb.addr, m_vs_cb.size);
		if (m_vs_pc.addr)
			dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, g_vs_pc_index, m_vs_pc.addr, m_vs_pc.size);
		if (m_ps_cb.addr)
			dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, g_ps_cb_index, m_ps_cb.addr, m_ps_cb.size);
		if (m_loose.addr)
		{
			dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, LOOSE_UNIFORM_BINDING, m_loose.addr, m_loose.size);
			dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, LOOSE_UNIFORM_BINDING, m_loose.addr, m_loose.size);
		}
	}
}

bool GSDeviceDK::IsBoundTextureUnit(const GSTextureDK* t) const
{
	for (const GSTextureDK* bound : m_textures)
	{
		if (bound == t)
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Transfers, used by GSTextureDK too

void GSDeviceDK::CommitClear(GSTexture* t)
{
	GSTextureDK* const T = static_cast<GSTextureDK*>(t);
	if (!T->IsRenderTargetOrDepthStencil() || T->GetState() == GSTexture::State::Dirty)
		return;
	if (T->GetState() == GSTexture::State::Invalidated)
	{
		T->SetState(GSTexture::State::Dirty);
		return;
	}

	// Bind the image alone, clear it, and leave the previous targets to be
	// bound again before the next draw.
	GSTextureDK* const saved[3] = {m_current_rt, m_current_ds_as_rt, m_current_ds};
	const bool saved_swapchain = m_rendering_to_swapchain;
	const GSVector2i saved_viewport = m_viewport;
	const GSVector4i saved_scissor = m_scissor;
	m_rendering_to_swapchain = false;
	m_current_rt = T->IsDepthStencil() ? nullptr : T;
	m_current_ds_as_rt = nullptr;
	m_current_ds = T->IsDepthStencil() ? T : nullptr;
	m_targets_dirty = true;
	SetViewport(T->GetSize());
	SetScissor(GSVector4i::loadh(T->GetSize()));
	FlushState();

	const DkCmdBuf cmd = m_ctx.Cmd();
	if (T->IsDepthStencil())
	{
		dkCmdBufClearDepthStencil(cmd, true, T->GetClearDepth(), 0, 0);
	}
	else
	{
		const GSVector4 c = T->GetClearForFormat();
		if (T->IsIntegerFormat())
			dkCmdBufClearColorUint(cmd, 0, DkColorMask_RGBA, c.U32[0], c.U32[1], c.U32[2], c.U32[3]);
		else
			dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, c.x, c.y, c.z, c.w);
	}
	T->m_last_write = m_epoch;
	T->SetState(GSTexture::State::Dirty);

	m_rendering_to_swapchain = saved_swapchain;
	m_current_rt = saved[0];
	m_current_ds_as_rt = saved[1];
	m_current_ds = saved[2];
	m_targets_dirty = true;
	SetViewport(saved_viewport);
	SetScissor(saved_scissor);
}

// The last transfers, printed when the GPU reports an error, to name the one
// that faulted.
void GSDeviceDK::TraceTransfer(const char* op, const GSTextureDK* a, const GSTextureDK* b, const GSVector4i& r, u64 addr, u32 pitch)
{
	TransferTrace& t = m_transfer_trace[m_transfer_trace_next++ % m_transfer_trace.size()];
	t = TransferTrace{op, m_ctx.CurrentBatch(), a ? a->GetSize() : GSVector2i(0, 0), b ? b->GetSize() : GSVector2i(0, 0), r, addr, pitch};
}

void GSDeviceDK::TraceDraw(u32 kind, u32 count)
{
	DrawTrace& d = m_draw_trace[m_draw_trace_next++ % m_draw_trace.size()];
	d.batch = m_ctx.CurrentBatch();
	d.kind = kind;
	d.count = count;
	d.vertices = m_vertex.count;
	d.vtx = m_vertex.addr;
	d.idx = m_index.addr;
	for (u32 i = 0; i < NUM_TEXTURE_UNITS; i++)
		d.slots[i] = m_textures[i] ? m_textures[i]->Descriptor() : 0;
	d.target = m_current_rt ? m_current_rt->GetSize() : (m_current_ds ? m_current_ds->GetSize() : GSVector2i(-1, -1));
}

void GSDeviceDK::DumpTransferTrace()
{
	Console.Error("deko3d: the last draws, oldest first:");
	for (size_t i = 0; i < m_draw_trace.size(); i++)
	{
		const DrawTrace& d = m_draw_trace[(m_draw_trace_next + i) % m_draw_trace.size()];
		if (!d.kind)
			continue;
		Console.Error("  batch %llu %s %u (of %u vertices at 0x%llx, indices 0x%llx) into %dx%d, slots %u %u %u %u %u",
			static_cast<unsigned long long>(d.batch), d.kind == 1 ? "indexed" : d.kind == 2 ? "plain" : "expanded", d.count, d.vertices,
			static_cast<unsigned long long>(d.vtx), static_cast<unsigned long long>(d.idx), d.target.x, d.target.y, d.slots[0], d.slots[1],
			d.slots[2], d.slots[3], d.slots[4]);
	}
	Console.Error("deko3d: the last transfers, oldest first:");
	for (size_t i = 0; i < m_transfer_trace.size(); i++)
	{
		const TransferTrace& t = m_transfer_trace[(m_transfer_trace_next + i) % m_transfer_trace.size()];
		if (!t.op)
			continue;
		Console.Error("  batch %llu %s: %dx%d -> %dx%d rect (%d,%d)-(%d,%d) buffer 0x%llx pitch %u", static_cast<unsigned long long>(t.batch), t.op,
			t.a.x, t.a.y, t.b.x, t.b.y, t.r.x, t.r.y, t.r.z, t.r.w, static_cast<unsigned long long>(t.addr), t.pitch);
	}
	std::fflush(stderr);
}

// A rectangle a copy engine can take: inside the image level, not empty.
static bool ClipToLevel(const GSTextureDK* t, int level, GSVector4i* r)
{
	const GSVector4i bounds(0, 0, std::max(1, t->GetWidth() >> level), std::max(1, t->GetHeight() >> level));
	*r = r->rintersect(bounds);
	return !r->rempty();
}

void GSDeviceDK::CopyBufferToTexture(GSTextureDK* t, int level, const GSVector4i& r_in, DkGpuAddr src, u32 pitch)
{
	GSVector4i r = r_in;
	if (!r.eq(r.rintersect(GSVector4i(0, 0, std::max(1, t->GetWidth() >> level), std::max(1, t->GetHeight() >> level)))) || r.rempty())
	{
		// Clipping an upload would need the source moved too; skip and say so.
		Console.Warning("deko3d: upload (%d,%d)-(%d,%d) does not fit a %dx%d texture level %d; skipped", r.x, r.y, r.z, r.w,
			t->GetWidth(), t->GetHeight(), level);
		return;
	}
	TraceTransfer("upload", nullptr, t, r, src, pitch);
	BeforeTransfer(nullptr, t);
	// DkCopyBuf's row length is in bytes.
	const DkCopyBuf buf = {src, pitch, 0};
	const DkImageView view = t->LevelView(level);
	const DkImageRect rect = Rect(r);
	dkCmdBufCopyBufferToImage(m_ctx.Cmd(), &buf, &view, &rect, 0);
	AfterTransfer(t);
}

void GSDeviceDK::CopyTextureToBuffer(GSTextureDK* t, int level, const GSVector4i& r_in, DkGpuAddr dst, u32 pitch)
{
	GSVector4i r = r_in;
	if (!ClipToLevel(t, level, &r))
		return;
	TraceTransfer("readback", t, nullptr, r, dst, pitch);
	BeforeTransfer(t, nullptr);
	const DkCopyBuf buf = {dst, pitch, 0};
	const DkImageView view = t->LevelView(level);
	const DkImageRect rect = Rect(r);
	dkCmdBufCopyImageToBuffer(m_ctx.Cmd(), &view, &rect, &buf, 0);
	AfterTransfer(nullptr);
}

void GSDeviceDK::GenerateMipmaps(GSTextureDK* t)
{
	CommitClear(t);
	for (int level = 1; level < t->GetMipmapLevels(); level++)
	{
		BeforeTransfer(t, t);
		const DkImageView src = t->LevelView(level - 1);
		const DkImageView dst = t->LevelView(level);
		const DkImageRect src_rect = {0, 0, 0, static_cast<u32>(std::max(1, t->GetWidth() >> (level - 1))),
			static_cast<u32>(std::max(1, t->GetHeight() >> (level - 1))), 1};
		const DkImageRect dst_rect = {0, 0, 0, static_cast<u32>(std::max(1, t->GetWidth() >> level)),
			static_cast<u32>(std::max(1, t->GetHeight() >> level)), 1};
		dkCmdBufBlitImage(m_ctx.Cmd(), &src, &src_rect, &dst, &dst_rect, DkBlitFlag_FilterLinear, 0);
		AfterTransfer(t);
	}
}

void GSDeviceDK::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r_in, u32 destX, u32 destY)
{
	// OpenGL clips a copy to both images; the copy engines read whatever the
	// rectangle says, past the image's memory too.
	GSVector4i r = r_in.rintersect(GSVector4i::loadh(sTex->GetSize()));
	r.z = std::min(r.z, r.x + std::max(0, dTex->GetWidth() - static_cast<int>(destX)));
	r.w = std::min(r.w, r.y + std::max(0, dTex->GetHeight() - static_cast<int>(destY)));
	if (r.rempty())
		return;
	GSTextureDK* const S = static_cast<GSTextureDK*>(sTex);
	GSTextureDK* const D = static_cast<GSTextureDK*>(dTex);
	if (dTex->IsDepthStencil())
		InvalidateDSAsRT(dTex);

	const GSVector4i dst_rect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	const bool full_draw_copy = dst_rect.eq(r);
	if (sTex->GetState() == GSTexture::State::Cleared)
	{
		if (dTex->IsRenderTargetOrDepthStencil() && ProcessClearsBeforeCopy(sTex, dTex, full_draw_copy))
			return;
		CommitClear(sTex);
	}
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	if (dTex->GetState() == GSTexture::State::Cleared && !full_draw_copy)
		CommitClear(dTex);

	if (sTex->IsDepthStencil() || dTex->IsDepthStencil())
	{
		// The copy engines do not take depth formats; copy with a draw.
		const GSVector4 sRect = GSVector4(r) / GSVector4(sTex->GetSize()).xyxy();
		const GSVector4 dRect(static_cast<float>(destX), static_cast<float>(destY),
			static_cast<float>(destX + r.width()), static_cast<float>(destY + r.height()));
		StretchRect(sTex, sRect, dTex, dRect, ShaderConvert::DEPTH_COPY, Nearest);
	}
	else
	{
		TraceTransfer("copy", S, D, r, (static_cast<u64>(destX) << 32) | destY, 0);
		BeforeTransfer(S, D);
		const DkImageView src = S->LevelView(0);
		const DkImageView dst = D->LevelView(0);
		const DkImageRect src_rect = Rect(r);
		const DkImageRect dst_rect = {destX, destY, 0, static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1};
		dkCmdBufCopyImage(m_ctx.Cmd(), &src, &src_rect, &dst, &dst_rect, 0);
		AfterTransfer(D);
	}
	dTex->SetState(GSTexture::State::Dirty);
}

// ---------------------------------------------------------------------------
// Geometry and draws

void GSDeviceDK::IASetVertexBuffer(const void* vertices, u32 stride, u32 count)
{
	const u32 size = stride * count;
	const dk::Context::Allocation alloc = m_ctx.Stream(size, 64);
	std::memcpy(alloc.cpu, vertices, size);
	m_vertex.addr = alloc.gpu;
	m_vertex.count = count;
	m_vertex.stride = stride;
}

void GSDeviceDK::IASetIndexBuffer(const void* index, u32 count)
{
	const u32 size = count * sizeof(u16);
	const dk::Context::Allocation alloc = m_ctx.Stream(size, 64);
	std::memcpy(alloc.cpu, index, size);
	m_index.addr = alloc.gpu;
	m_index.count = count;
}

void GSDeviceDK::VSSetPushConstants(u32 base_vertex, u32 base_index)
{
	GSHWDrawConfig::VSPushConstants pc;
	pc.base_vertex = base_vertex;
	pc.base_index = base_index;
	const dk::Context::Allocation alloc = m_ctx.Stream(sizeof(pc), DK_UNIFORM_BUF_ALIGNMENT);
	std::memcpy(alloc.cpu, &pc, sizeof(pc));
	m_vs_pc = DkBufExtents{alloc.gpu, sizeof(pc)};
	m_ubos_dirty = true;
}

void GSDeviceDK::DrawPrimitive()
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	TraceDraw(2, m_vertex.count);
	const DkCmdBuf cmd = m_ctx.Cmd();
	dkCmdBufBindVtxBuffer(cmd, 0, m_vertex.addr, m_vertex.count * m_vertex.stride);
	dkCmdBufDraw(cmd, m_topology, m_vertex.count, 1, 0, 0);
}

void GSDeviceDK::DrawIndexedPrimitive(int offset, int count)
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	TraceDraw(1, static_cast<u32>(count));
	const DkCmdBuf cmd = m_ctx.Cmd();
	dkCmdBufBindVtxBuffer(cmd, 0, m_vertex.addr, m_vertex.count * m_vertex.stride);
	dkCmdBufBindIdxBuffer(cmd, DkIdxFormat_Uint16, m_index.addr);
	dkCmdBufDrawIndexed(cmd, m_topology, static_cast<u32>(count), 1, static_cast<u32>(offset), 0, 0);
}

void GSDeviceDK::Draw(const GSHWDrawConfig& config, int offset, int count)
{
	const DkCmdBuf cmd = m_ctx.Cmd();
	if (config.vs.expand != GSHWDrawConfig::VSExpand::None)
	{
		// The vertex shader reads vertices (and indices) from storage buffers.
		g_perfmon.Put(GSPerfMon::DrawCalls, 1);
		TraceDraw(3, static_cast<u32>(count));
		const bool vs_indexing = config.vs.UseVSExpandIndexBuffer();
		VSSetPushConstants(0, vs_indexing ? static_cast<u32>(offset) : 0);
		FlushState();
		dkCmdBufBindStorageBuffer(cmd, DkStage_Vertex, g_vs_vb_index, m_vertex.addr, m_vertex.count * m_vertex.stride);
		if (vs_indexing)
		{
			dkCmdBufBindStorageBuffer(cmd, DkStage_Vertex, g_vs_ib_index, m_index.addr, (m_index.count * sizeof(u16) + 3u) & ~3u);
			dkCmdBufDraw(cmd, m_topology, static_cast<u32>(count) * GetExpansionFactor(config.vs.expand), 1, 0, 0);
		}
		else
		{
			// Points and sprites use the fixed expansion indices; lines use the
			// draw's own (as GSDeviceOGL's VAOs do). Line expansion with the
			// fixed buffer read vertices ~4 GB past the ring and faulted the GPU.
			const DkGpuAddr ib = config.vs.UseFixedExpandIndexBuffer() ? m_expand_ib_gpu : m_index.addr;
			dkCmdBufBindIdxBuffer(cmd, DkIdxFormat_Uint16, ib);
			dkCmdBufDrawIndexed(cmd, m_topology, static_cast<u32>(count), 1, static_cast<u32>(offset), 0, 0);
		}
	}
	else
	{
		FlushState();
		DrawIndexedPrimitive(offset, count);
	}
	NoteDrawWrites();
}

void GSDeviceDK::DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	const float inv_x = 2.0f / ds.x;
	const float inv_y = 2.0f / ds.y;
	const float left = dRect.x * inv_x - 1.0f;
	const float right = dRect.z * inv_x - 1.0f;
	const float top = -1.0f + dRect.y * inv_y;
	const float bottom = -1.0f + dRect.w * inv_y;
	const GSVertexPT1 vertices[] = {
		{GSVector4(left, top, 0.0f, 0.0f), GSVector2(sRect.x, sRect.y)},
		{GSVector4(right, top, 0.0f, 0.0f), GSVector2(sRect.z, sRect.y)},
		{GSVector4(left, bottom, 0.0f, 0.0f), GSVector2(sRect.x, sRect.w)},
		{GSVector4(right, bottom, 0.0f, 0.0f), GSVector2(sRect.z, sRect.w)},
	};
	SetVertexLayout(LAYOUT_GS);
	IASetVertexBuffer(vertices, sizeof(GSVertexPT1), 4);
	SetTopology(DkPrimitive_TriangleStrip);
	FlushState();
	DrawPrimitive();
	NoteDrawWrites();
}

void GSDeviceDK::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvertSelector shader, Filter filter)
{
	const u8 mask = shader.Mask();
	shader = shader.SetMask();
	filter = shader.SupportsBilinear() ? Nearest : filter;
	DoStretchRect(sTex, sRect, dTex, dRect, m_convert[shader.Index()], false, OMColorMaskSelector(mask), filter);
}

void GSDeviceDK::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	const Program& prog, bool alpha_blend, OMColorMaskSelector cms, Filter filter)
{
	if (!prog.Valid())
		return;
	CommitClear(sTex);
	const bool draw_in_depth = dTex->IsDepthStencil();
	if (draw_in_depth)
		OMSetRenderTargets(nullptr, nullptr, dTex);
	else
		OMSetRenderTargets(dTex, nullptr, nullptr);
	BindProgram(prog);
	OMSetDepthStencil(draw_in_depth ? DSS_CONVERT_WRITE : DSS_CONVERT);
	OMSetBlendSimple(alpha_blend, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendOp_Add);
	OMSetColorMask(cms);
	ClearTextureUnits();
	PSSetShaderResource(TEXTURE_TEXTURE, sTex);
	PSSetSampler(filter == Biln ? m_sampler_linear : SAMPLER_POINT);
	DrawStretchRect(sRect, dRect, dTex->GetSize());
}

void GSDeviceDK::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, PresentShader shader, float shaderTime, Filter filter)
{
	const Program& prog = m_present[static_cast<int>(shader)];
	if (!prog.Valid())
		return;
	CommitClear(sTex);
	if (dTex)
		OMSetRenderTargets(dTex, nullptr, nullptr);
	const GSVector2i ds(dTex ? dTex->GetSize() : GetWindowSize());
	DisplayConstantBuffer cb;
	cb.SetSource(sRect, sTex->GetSize());
	cb.SetTarget(dRect, ds);
	cb.SetTime(shaderTime);
	// present.glsl's loose uniforms are laid out exactly like this buffer.
	SetLooseUniforms(&cb, sizeof(cb));
	BindProgram(prog);
	OMSetDepthStencil(DSS_CONVERT);
	OMSetBlend(BlendState());
	OMSetColorMask(OMColorMaskSelector());
	ClearTextureUnits();
	PSSetShaderResource(TEXTURE_TEXTURE, sTex);
	PSSetSampler(filter == Biln ? m_sampler_linear : SAMPLER_POINT);
	// As OpenGL: the window shows memory row 0 at the bottom (measured with
	// the orientation probe), so the source is flipped.
	DrawStretchRect(GSVector4(sRect.xwzy()), dRect, ds);
}

void GSDeviceDK::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{
	CommitClear(sTex);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	const Program& prog = m_convert[ShaderConvertSelector(shader).Index()];
	if (!prog.Valid())
		return;
	struct alignas(16)
	{
		u32 offset[3];
		float scale;
	} uniforms = {{offsetX, offsetY, dOffset}, sScale};
	SetLooseUniforms(&uniforms, sizeof(uniforms));
	OMSetRenderTargets(dTex, nullptr, nullptr);
	BindProgram(prog);
	OMSetDepthStencil(DSS_CONVERT);
	OMSetBlend(BlendState());
	OMSetColorMask(OMColorMaskSelector());
	ClearTextureUnits();
	PSSetShaderResource(TEXTURE_TEXTURE, sTex);
	PSSetSampler(SAMPLER_POINT);
	DrawStretchRect(GSVector4::zero(), GSVector4(0, 0, dSize, 1), dTex->GetSize());
}

void GSDeviceDK::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	CommitClear(sTex);
	const ShaderConvert shader = ((SPSM & 0xE) == 0) ? ShaderConvert::RGBA_TO_8I : ShaderConvert::RGB5A1_TO_8I;
	const Program& prog = m_convert[ShaderConvertSelector(shader).Index()];
	if (!prog.Valid())
		return;
	struct alignas(16)
	{
		u32 sbw, dbw, psm;
		float scale;
	} uniforms = {SBW, DBW, SPSM, sScale};
	SetLooseUniforms(&uniforms, sizeof(uniforms));
	OMSetRenderTargets(dTex, nullptr, nullptr);
	BindProgram(prog);
	OMSetDepthStencil(DSS_CONVERT);
	OMSetBlend(BlendState());
	OMSetColorMask(OMColorMaskSelector());
	ClearTextureUnits();
	PSSetShaderResource(TEXTURE_TEXTURE, sTex);
	PSSetSampler(SAMPLER_POINT);
	DrawStretchRect(GSVector4::zero(), GSVector4(0, 0, dTex->GetWidth(), dTex->GetHeight()), dTex->GetSize());
}

void GSDeviceDK::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{
	CommitClear(sTex);
	const Program& prog = m_convert[ShaderConvertSelector(ShaderConvert::DOWNSAMPLE_COPY).Index()];
	if (!prog.Valid())
		return;
	struct alignas(16)
	{
		s32 clamp_min[2];
		s32 factor;
		float weight;
		float step;
	} uniforms = {{clamp_min.x, clamp_min.y}, static_cast<s32>(downsample_factor),
		static_cast<float>(downsample_factor * downsample_factor),
		(GSConfig.UserHacks_NativeScaling > GSNativeScaling::Aggressive) ? 2.0f : 1.0f};
	SetLooseUniforms(&uniforms, sizeof(uniforms));
	OMSetRenderTargets(dTex, nullptr, nullptr);
	BindProgram(prog);
	OMSetDepthStencil(DSS_CONVERT);
	OMSetBlend(BlendState());
	OMSetColorMask(OMColorMaskSelector());
	ClearTextureUnits();
	PSSetShaderResource(TEXTURE_TEXTURE, sTex);
	PSSetSampler(SAMPLER_POINT);
	DrawStretchRect(GSVector4::zero(), dRect, dTex->GetSize());
}

void GSDeviceDK::DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvertSelector shader)
{
	shader = shader.SetMask();
	const Program& prog = m_convert[shader.Index()];
	if (!prog.Valid())
		return;
	if (!dTex->IsDepthStencil())
		OMSetRenderTargets(dTex, nullptr, nullptr);
	else
		OMSetRenderTargets(nullptr, nullptr, dTex);
	BindProgram(prog);
	OMSetDepthStencil(shader.DepthOutput() ? DSS_CONVERT_WRITE : DSS_CONVERT);
	OMSetBlend(BlendState());
	ClearTextureUnits();

	const GSVector2 ds(static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	GSTexture* last_tex = rects[0].src;
	Filter last_filter = rects[0].filter;
	u8 last_wmask = rects[0].wmask.wrgba;
	u32 first = 0;
	u32 count = 1;
	for (u32 i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].filter == last_filter && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}
		DoMultiStretchRects(rects + first, count, ds);
		last_tex = rects[i].src;
		last_filter = rects[i].filter;
		last_wmask = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}
	DoMultiStretchRects(rects + first, count, ds);
}

void GSDeviceDK::DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, const GSVector2& ds)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	std::vector<GSVertexPT1> verts;
	std::vector<u16> idx;
	verts.reserve(num_rects * 4);
	idx.reserve(num_rects * 6);
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;
		const float inv_x = 2.0f / ds.x;
		const float inv_y = 2.0f / ds.y;
		const float left = dRect.x * inv_x - 1.0f;
		const float right = dRect.z * inv_x - 1.0f;
		const float top = -1.0f + dRect.y * inv_y;
		const float bottom = -1.0f + dRect.w * inv_y;
		const u16 vstart = static_cast<u16>(verts.size());
		verts.push_back({GSVector4(left, top, 0.0f, 0.0f), GSVector2(sRect.x, sRect.y)});
		verts.push_back({GSVector4(right, top, 0.0f, 0.0f), GSVector2(sRect.z, sRect.y)});
		verts.push_back({GSVector4(left, bottom, 0.0f, 0.0f), GSVector2(sRect.x, sRect.w)});
		verts.push_back({GSVector4(right, bottom, 0.0f, 0.0f), GSVector2(sRect.z, sRect.w)});
		if (i > 0)
			idx.push_back(vstart);
		idx.push_back(vstart);
		idx.push_back(vstart + 1);
		idx.push_back(vstart + 2);
		idx.push_back(vstart + 3);
		idx.push_back(vstart + 3);
	}
	for (u32 i = 0; i < num_rects; i++)
		CommitClear(rects[i].src);
	SetVertexLayout(LAYOUT_GS);
	IASetVertexBuffer(verts.data(), sizeof(GSVertexPT1), static_cast<u32>(verts.size()));
	IASetIndexBuffer(idx.data(), static_cast<u32>(idx.size()));
	SetTopology(DkPrimitive_TriangleStrip);
	PSSetShaderResource(TEXTURE_TEXTURE, rects[0].src);
	PSSetSampler(rects[0].filter == Biln ? m_sampler_linear : SAMPLER_POINT);
	OMSetColorMask(rects[0].wmask);
	FlushState();
	DrawIndexedPrimitive(0, static_cast<int>(idx.size()));
	NoteDrawWrites();
}

void GSDeviceDK::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter)
{
	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;

	OMSetColorMask(OMColorMaskSelector());
	ClearRenderTarget(dTex, c);

	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
		StretchRect(sTex[1], sRect[1], dTex, PMODE.SLBG ? dRect[2] : dRect[1], ShaderConvert::COPY, filter);

	if (feedback_write_2 || feedback_write_1)
	{
		const s32 emod[4] = {static_cast<s32>(EXTBUF.EMODA), static_cast<s32>(EXTBUF.EMODC), 0, 0};
		SetLooseUniforms(emod, sizeof(emod));
	}
	if (feedback_write_2)
		StretchRect(dTex, full_r, sTex[2], dRect[2], ShaderConvert::YUV, filter);
	if (feedback_write_2_but_blend_bg)
		ClearRenderTarget(dTex, c);

	if (sTex[0])
	{
		const OMColorMaskSelector mask(PMODE.AMOD == 1 ? 0x7 : 0xF);
		if (PMODE.MMOD == 1)
		{
			const GSVector4 bg = GSVector4::unorm8(c);
			SetLooseUniforms(&bg, sizeof(bg));
			DoStretchRect(sTex[0], sRect[0], dTex, dRect[0], m_merge[1], true, mask, filter);
		}
		else
		{
			DoStretchRect(sTex[0], sRect[0], dTex, dRect[0], m_merge[0], true, mask, filter);
		}
	}

	if (feedback_write_1)
	{
		const s32 emod[4] = {static_cast<s32>(EXTBUF.EMODA), static_cast<s32>(EXTBUF.EMODC), 0, 0};
		SetLooseUniforms(emod, sizeof(emod));
		StretchRect(dTex, full_r, sTex[2], dRect[2], ShaderConvert::YUV, filter);
	}
}

void GSDeviceDK::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb)
{
	SetLooseUniforms(cb.ZrH.F32, sizeof(cb.ZrH));
	DoStretchRect(sTex, sRect, dTex, dRect, m_interlace[static_cast<int>(shader)], false, OMColorMaskSelector(), filter);
}

void GSDeviceDK::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
	const GSVector2i s = dTex->GetSize();
	DoStretchRect(sTex, GSVector4(0, 0, 1, 1), dTex, GSVector4(0, 0, s.x, s.y), m_fxaa, false, OMColorMaskSelector(), Biln);
}

void GSDeviceDK::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
	SetLooseUniforms(params, sizeof(float) * 4);
	const GSVector2i s = dTex->GetSize();
	DoStretchRect(sTex, GSVector4(0, 0, 1, 1), dTex, GSVector4(0, 0, s.x, s.y), m_shadeboost, false, OMColorMaskSelector(), Nearest);
}

bool GSDeviceDK::DoCAS(GSTexture*, GSTexture*, bool, const std::array<u32, NUM_CAS_CONSTANTS>&)
{
	return false;
}

void GSDeviceDK::ClearSamplerCache()
{
	for (u32 key = 0; key < 256; key++)
		m_ctx.WriteSamplerDescriptor(key, MakeSampler(PSSamplerSelector(key)));
}

// ---------------------------------------------------------------------------
// Hardware draws

void GSDeviceDK::SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox)
{
	const Program& prog = m_convert[ShaderConvertSelector(SetDATMShader(datm)).Index()];
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	const GSVector4i saved_scissor = m_scissor;
	OMSetRenderTargets(nullptr, nullptr, ds, &saved_scissor);
	FlushState();
	dkCmdBufClearDepthStencil(m_ctx.Cmd(), false, 0.0f, 0xFF, 0);
	if (!prog.Valid())
		return;
	BindProgram(prog);
	OMSetDepthStencil(DSS_DATE);
	OMSetBlend(BlendState());
	OMSetColorMask(OMColorMaskSelector());

	const GSVector4 src = GSVector4(bbox) / GSVector4(ds->GetSize()).xyxy();
	const GSVector4 dst = src * 2.f - 1.f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, dst.y, 0.0f, 0.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, dst.y, 0.0f, 0.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, dst.w, 0.0f, 0.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, dst.w, 0.0f, 0.0f), GSVector2(src.z, src.w)},
	};
	SetVertexLayout(LAYOUT_GS);
	IASetVertexBuffer(vertices, sizeof(GSVertexPT1), 4);
	SetTopology(DkPrimitive_TriangleStrip);
	ClearTextureUnits();
	PSSetShaderResource(TEXTURE_TEXTURE, rt);
	PSSetSampler(SAMPLER_POINT);
	FlushState();
	DrawPrimitive();
	NoteDrawWrites();
}

GSTexture* GSDeviceDK::InitPrimDateTexture(GSTexture* rt, const GSVector4i& area, SetDATM datm)
{
	const GSVector2i& rtsize = rt->GetSize();
	GSTexture* tex = CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false);
	if (!tex)
		return nullptr;
	DoStretchRect(rt, GSVector4(area) / GSVector4(rtsize).xyxy(), tex, GSVector4(area), m_date_primid[static_cast<u8>(datm)], false, OMColorMaskSelector(), Nearest);
	return tex;
}

void GSDeviceDK::SendHWDraw(const GSHWDrawConfig& config, bool one_barrier, bool full_barrier)
{
	if (full_barrier)
	{
		const u32 indices_per_prim = config.indices_per_prim;
		const u32 draw_list_size = static_cast<u32>(config.drawlist->size());
		for (u32 n = 0, p = 0; n < draw_list_size; n++)
		{
			const u32 count = config.drawlist->at(n) * indices_per_prim;
			FlushState();
			TextureBarrier();
			Draw(config, static_cast<int>(p), static_cast<int>(count));
			p += count;
		}
		return;
	}
	if (one_barrier)
	{
		FlushState();
		TextureBarrier();
	}
	Draw(config);
}

void GSDeviceDK::RenderHW(GSHWDrawConfig& config)
{
	Common::Timer timer;
	ScopedGuard timed([&timer]() { g_un5_render_ms += timer.GetTimeMilliseconds(); });

	if (config.tex)
		CommitClear(config.tex);
	if (config.pal)
		CommitClear(config.pal);

	const GSVector2i rtsize = (config.rt ? config.rt : config.ds)->GetSize();
	GSTexture* colclip_rt = g_gs_device->GetColorClipTexture();
	GSTexture* draw_rt = config.rt;
	GSTexture* draw_ds = config.ds;
	GSTexture* draw_ds_as_rt = config.ps.IsFeedbackLoopDepth() ? m_ds_as_rt : nullptr;
	GSTexture* primid_texture = nullptr;
	ScopedGuard recycle_temp_textures([&]() {
		if (primid_texture)
			Recycle(primid_texture);
	});

	if (colclip_rt)
	{
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::EarlyResolve)
		{
			const GSVector2i size = config.rt->GetSize();
			const GSVector4 dRect(config.colclip_update_area);
			const GSVector4 sRect = dRect / GSVector4(size.x, size.y).xyxy();
			StretchRect(colclip_rt, sRect, config.rt, dRect, ShaderConvert::COLCLIP_RESOLVE, Nearest);
			Recycle(colclip_rt);
			g_gs_device->SetColorClipTexture(nullptr);
			colclip_rt = nullptr;
		}
		else
		{
			config.ps.colclip_hw = 1;
		}
	}
	if (config.ps.colclip_hw)
	{
		if (!colclip_rt)
		{
			config.colclip_update_area = config.drawarea;
			colclip_rt = CreateFeedbackTarget(rtsize.x, rtsize.y, GSTexture::Format::ColorClip, false);
			if (!colclip_rt)
			{
				Console.Warning("deko3d: failed to allocate the ColorClip target, draw skipped");
				return;
			}
			g_gs_device->SetColorClipTexture(colclip_rt);
			const GSVector4 dRect = GSVector4((config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly) ? GSVector4i::loadh(rtsize) : config.drawarea);
			const GSVector4 sRect = dRect / GSVector4(rtsize.x, rtsize.y).xyxy();
			StretchRect(config.rt, sRect, colclip_rt, dRect, ShaderConvert::COLCLIP_INIT, Nearest);
		}
		draw_rt = colclip_rt;
	}

	const bool need_barrier = config.require_one_barrier || (config.require_full_barrier && m_features.feedback_loops());
	switch (config.destination_alpha)
	{
		case GSHWDrawConfig::DestinationAlphaMode::Off:
		case GSHWDrawConfig::DestinationAlphaMode::Full:
			break;
		case GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking:
			primid_texture = InitPrimDateTexture(colclip_rt ? colclip_rt : config.rt, config.drawarea, config.datm);
			if (!primid_texture)
			{
				Console.Warning("deko3d: failed to allocate the DATE image, draw skipped");
				return;
			}
			break;
		case GSHWDrawConfig::DestinationAlphaMode::StencilOne:
			if (need_barrier)
				break; // cleared after the targets are bound
			[[fallthrough]];
		case GSHWDrawConfig::DestinationAlphaMode::Stencil:
			SetupDATE(colclip_rt ? colclip_rt : config.rt, config.ds, config.datm, config.drawarea);
			break;
	}

	// Geometry.
	IASetVertexBuffer(config.verts, sizeof(GSVertex), config.nverts);
	if (config.vs.UseFixedExpandIndexBuffer())
	{
		SetVertexLayout(LAYOUT_NONE);
		m_index.count = config.nindices;
		if (config.nindices > EXPAND_BUFFER_SIZE / sizeof(u16))
		{
			// More than the fixed buffer holds: the GPU would read past it.
			static bool s_warned = false;
			if (!s_warned)
				Console.Error("deko3d: %u expanded indices, more than the %u the buffer holds; draw cut", config.nindices,
					static_cast<u32>(EXPAND_BUFFER_SIZE / sizeof(u16)));
			s_warned = true;
			m_index.count = EXPAND_BUFFER_SIZE / sizeof(u16);
		}
	}
	else if (config.vs.UseVSExpandIndexBuffer())
	{
		SetVertexLayout(LAYOUT_NONE);
		IASetIndexBuffer(config.indices, config.nindices);
	}
	else
	{
		SetVertexLayout(config.vs.expand == GSHWDrawConfig::VSExpand::None ? LAYOUT_GS : LAYOUT_NONE);
		IASetIndexBuffer(config.indices, config.nindices);
	}
	switch (config.topology)
	{
		case GSHWDrawConfig::Topology::Point: SetTopology(DkPrimitive_Points); break;
		case GSHWDrawConfig::Topology::Line: SetTopology(DkPrimitive_Lines); break;
		case GSHWDrawConfig::Topology::Triangle: SetTopology(DkPrimitive_Triangles); break;
	}

	// Textures.
	ClearTextureUnits();
	if (config.tex)
		PSSetShaderResource(TEXTURE_TEXTURE, config.tex);
	if (config.pal)
		PSSetShaderResource(TEXTURE_PALETTE, config.pal);
	if (config.require_one_barrier || config.require_full_barrier)
	{
		PSSetShaderResource(TEXTURE_RT, colclip_rt ? colclip_rt : config.rt);
		if (config.ps.IsFeedbackLoopDepth())
			PSSetShaderResource(TEXTURE_DEPTH, m_features.depth_feedback ? config.ds : draw_ds_as_rt);
	}
	PSSetSampler(config.sampler.key);

	// Uniforms.
	if (m_vs_cb_cache.Update(config.cb_vs))
	{
		const dk::Context::Allocation alloc = m_ctx.Stream(sizeof(m_vs_cb_cache), DK_UNIFORM_BUF_ALIGNMENT);
		std::memcpy(alloc.cpu, &m_vs_cb_cache, sizeof(m_vs_cb_cache));
		m_vs_cb = DkBufExtents{alloc.gpu, sizeof(m_vs_cb_cache)};
		m_ubos_dirty = true;
	}
	if (m_ps_cb_cache.Update(config.cb_ps))
	{
		const dk::Context::Allocation alloc = m_ctx.Stream(sizeof(m_ps_cb_cache), DK_UNIFORM_BUF_ALIGNMENT);
		std::memcpy(alloc.cpu, &m_ps_cb_cache, sizeof(m_ps_cb_cache));
		m_ps_cb = DkBufExtents{alloc.gpu, sizeof(m_ps_cb_cache)};
		m_ubos_dirty = true;
	}

	// Program. A shader missing from the pack skips the draw.
	const DkShader* vs = GetTfxVS(config.vs);
	PSSelector ps_sel = config.ps;
	const DkShader* ps = GetTfxPS(ps_sel);
	if (!vs || !ps)
		return;
	BindProgram(Program{vs, ps});

	if (config.topology == GSHWDrawConfig::Topology::Line)
	{
		const float line_width = config.line_expand ? config.cb_ps.ScaleFactor.z : 1.0f;
		if (m_line_width != line_width)
		{
			m_line_width = line_width;
			m_line_width_dirty = true;
		}
	}

	if (primid_texture)
	{
		OMSetRenderTargets(primid_texture, nullptr, config.ds, &config.scissor);
		OMColorMaskSelector mask;
		mask.wrgba = 0;
		mask.wr = true;
		OMSetColorMask(mask);
		OMSetBlendSimple(true, DkBlendFactor_One, DkBlendFactor_One, DkBlendOp_Min);
		OMDepthStencilSelector dss = config.depth;
		dss.zwe = 0;
		OMSetDepthStencil(dss.key);
		Draw(config);

		ps_sel.date = 3;
		config.alpha_second_pass.ps.date = 3;
		ps = GetTfxPS(ps_sel);
		if (!ps)
			return;
		BindProgram(Program{vs, ps});
		PSSetShaderResource(TEXTURE_PRIMID, primid_texture);
	}

	if (draw_ds_as_rt)
		OMSetBlendSimple(true, DkBlendFactor_One, DkBlendFactor_Zero, DkBlendOp_Add);
	SetBlendFromConfig(config.blend, config.colormask);

	OMSetRenderTargets(draw_rt, draw_ds_as_rt, draw_ds, &config.scissor);
	OMSetColorMask(config.colormask);
	OMSetDepthStencil(config.depth.key);

	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne && need_barrier)
	{
		FlushState();
		dkCmdBufClearDepthStencil(m_ctx.Cmd(), false, 0.0f, 0xFF, 1);
	}

	SendHWDraw(config, config.require_one_barrier, config.require_full_barrier);

	if (config.blend_multi_pass.enable)
	{
		SetBlendFromConfig(config.blend_multi_pass.blend, config.colormask);
		ps_sel.no_color1 = config.blend_multi_pass.no_color1;
		ps_sel.blend_hw = config.blend_multi_pass.blend_hw;
		ps_sel.dither = config.blend_multi_pass.dither;
		if (const DkShader* mp = GetTfxPS(ps_sel))
		{
			BindProgram(Program{vs, mp});
			Draw(config);
		}
	}

	if (config.alpha_second_pass.enable)
	{
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			m_ps_cb_cache = config.cb_ps;
			const dk::Context::Allocation alloc = m_ctx.Stream(sizeof(m_ps_cb_cache), DK_UNIFORM_BUF_ALIGNMENT);
			std::memcpy(alloc.cpu, &m_ps_cb_cache, sizeof(m_ps_cb_cache));
			m_ps_cb = DkBufExtents{alloc.gpu, sizeof(m_ps_cb_cache)};
			m_ubos_dirty = true;
		}
		if (const DkShader* second = GetTfxPS(config.alpha_second_pass.ps))
		{
			BindProgram(Program{vs, second});
			OMSetColorMask(config.alpha_second_pass.colormask);
			SetBlendFromConfig(config.blend, config.alpha_second_pass.colormask);
			OMSetDepthStencil(config.alpha_second_pass.depth.key);
			const bool one_barrier = config.alpha_second_pass.require_one_barrier && m_features.feedback_loops();
			SendHWDraw(config, one_barrier, config.alpha_second_pass.require_full_barrier);
		}
	}

	if (colclip_rt)
	{
		config.colclip_update_area = config.colclip_update_area.runion(config.drawarea);
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve)
		{
			const GSVector2i size = config.rt->GetSize();
			const GSVector4 dRect(config.colclip_update_area);
			const GSVector4 sRect = dRect / GSVector4(size.x, size.y).xyxy();
			StretchRect(colclip_rt, sRect, config.rt, dRect, ShaderConvert::COLCLIP_RESOLVE, Nearest);
			Recycle(colclip_rt);
			g_gs_device->SetColorClipTexture(nullptr);
		}
	}
}

const DkShader* GSDeviceDK::GetTfxVS(VSSelector sel)
{
	if (const auto it = m_vs_cache.find(sel.key); it != m_vs_cache.end())
		return it->second;
	const DkShader* shader = GetShader(DkStage_Vertex, GetVSSource(sel));
	m_vs_cache.emplace(sel.key, shader);
	return shader;
}

const DkShader* GSDeviceDK::GetTfxPS(const PSSelector& sel)
{
	if (const auto it = m_ps_cache.find(sel); it != m_ps_cache.end())
		return it->second;
	const DkShader* shader = GetShader(DkStage_Fragment, GetPSSource(sel));
	m_ps_cache.emplace(sel, shader);
	return shader;
}

// ---------------------------------------------------------------------------
// Presenting

GSDevice::PresentResult GSDeviceDK::BeginPresent(bool frame_skip)
{
	if (frame_skip)
		return PresentResult::FrameSkipped;
	m_swap_slot = dkQueueAcquireImage(m_ctx.Queue(), m_swapchain);
	if (g_un5_presents < 3)
	{
		Console.WriteLn("deko3d: present %llu, swapchain image %d", static_cast<unsigned long long>(g_un5_presents), m_swap_slot);
		std::fflush(stderr);
	}
	BindSwapchainTarget();
	OMSetColorMask(OMColorMaskSelector());
	FlushState();
	dkCmdBufClearColorFloat(m_ctx.Cmd(), 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
	return PresentResult::OK;
}

void GSDeviceDK::EndPresent()
{
	RenderImGui();

	Common::Timer swap;
	const dk::BatchId frame = m_ctx.CurrentBatch();
	m_ctx.Submit();
	if (m_ctx.InErrorState())
	{
		Console.Error("deko3d: stopping after a GPU error (see above)");
		std::fflush(stderr);
		std::abort();
	}
	dkQueuePresentImage(m_ctx.Queue(), m_swapchain, m_swap_slot);
	g_un5_swap_ms += swap.GetTimeMilliseconds();

	// Is the GPU still on the last frame when this one is presented?
	if (m_frame_batch && !m_ctx.IsComplete(m_frame_batch))
		g_un5_gpu_behind++;
	m_frame_batch = frame;
	g_un5_presents++;

	m_rendering_to_swapchain = false;
	m_targets_dirty = true;
}

void GSDeviceDK::RenderImGui()
{
	if (!ImGui::GetCurrentContext())
		return;
	ImGui::Render();
	const ImDrawData* draw_data = ImGui::GetDrawData();
	if (draw_data->CmdLists.Size == 0 || !m_imgui.Valid())
		return;
	UpdateImGuiTextures();

	// OpenGL's projection: ImGui's y runs down from the top, the window's
	// row 0 is at the bottom.
	const float W = static_cast<float>(m_window_info.surface_width);
	const float H = static_cast<float>(m_window_info.surface_height);
	const float ortho_projection[4][4] = {
		{2.0f / W, 0.0f, 0.0f, 0.0f},
		{0.0f, -2.0f / H, 0.0f, 0.0f},
		{0.0f, 0.0f, -1.0f, 0.0f},
		{-1.0f, 1.0f, 0.0f, 1.0f},
	};
	SetLooseUniforms(ortho_projection, sizeof(ortho_projection));
	BindSwapchainTarget();
	BindProgram(m_imgui);
	SetVertexLayout(LAYOUT_IMGUI);
	SetTopology(DkPrimitive_Triangles);
	OMSetBlendSimple(true, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendOp_Add);
	OMSetDepthStencil(DSS_CONVERT);
	OMSetColorMask(OMColorMaskSelector());
	ClearTextureUnits();
	PSSetSampler(m_sampler_linear);

	const DkCmdBuf cmd = m_ctx.Cmd();
	for (int n = 0; n < draw_data->CmdLists.Size; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];
		IASetVertexBuffer(cmd_list->VtxBuffer.Data, sizeof(ImDrawVert), static_cast<u32>(cmd_list->VtxBuffer.Size));
		IASetIndexBuffer(cmd_list->IdxBuffer.Data, static_cast<u32>(cmd_list->IdxBuffer.Size));
		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
			const GSVector4 clip = GSVector4::load<false>(&pcmd->ClipRect);
			if ((clip.zwzw() <= clip.xyxy()).mask() != 0)
				continue;
			const GSVector4i iclip(clip);
			SetScissor(GSVector4i(iclip.x, static_cast<int>(H) - iclip.w, iclip.z, static_cast<int>(H) - iclip.y));
			PSSetShaderResource(TEXTURE_TEXTURE, reinterpret_cast<GSTextureDK*>(pcmd->GetTexID()));
			FlushState();
			dkCmdBufBindVtxBuffer(cmd, 0, m_vertex.addr, m_vertex.count * m_vertex.stride);
			dkCmdBufBindIdxBuffer(cmd, DkIdxFormat_Uint16, m_index.addr);
			dkCmdBufDrawIndexed(cmd, DkPrimitive_Triangles, pcmd->ElemCount, 1, pcmd->IdxOffset, static_cast<s32>(pcmd->VtxOffset), 0);
		}
		g_perfmon.Put(GSPerfMon::DrawCalls, cmd_list->CmdBuffer.Size);
	}
	SetVertexLayout(LAYOUT_GS);
}
