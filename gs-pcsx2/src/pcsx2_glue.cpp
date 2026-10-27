// SPDX-License-Identifier: GPL-3.0+
// What PCSX2's GS expects from the rest of PCSX2 (its VM, settings, OSD,
// captures, dumps, texture replacements), for un5-nx: defaults, or nothing.
// GSOptions' constructor and GSGetRGBA8AlphaMinMax are copied from PCSX2
// (Pcsx2Config.cpp and GS.cpp at the pinned commit).
#include "Config.h"
#include "GS/GS.h"
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSPng.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "PerformanceMetrics.h"
#include "VMManager.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/HostSys.h"
#include "common/Image.h"
#include "common/Path.h"
#include "imgui_freetype.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>

// ---- settings ----------------------------------------------------------

Pcsx2Config::GSOptions::GSOptions()
{
	bitsets[0] = 0;
	bitsets[1] = 0;

	PCRTCAntiBlur = true;
	DisableInterlaceOffset = false;
	PCRTCOffsets = false;
	PCRTCOverscan = false;
	IntegerScaling = false;
	LinearPresent = GSPostBilinearMode::BilinearSmooth;
	UseDebugDevice = false;
	UseBlitSwapChain = false;
	DisableShaderCache = false;
	DisableFramebufferFetch = false;
	DisableVertexShaderExpand = false;
	SkipDuplicateFrames = true;
	AdvancedFrameDisplay = false;
	OsdMessagesPos = OsdOverlayPos::TopLeft;
	OsdPerformancePos = OsdOverlayPos::TopRight;
	OsdShowSpeed = false;
	OsdShowFPS = false;
	OsdShowVPS = false;
	OsdShowResolution = false;
	OsdShowGSStats = false;
	OsdShowCPU = false;
	OsdShowGPU = false;
	OsdShowGPUDebug = false;
	OsdShowGPUStats = false;
	OsdShowGPUShadersStats = false;
	OsdShowIndicators = true;
	OsdShowFrameTimes = false;
	OsdShowHardwareInfo = false;
	OsdShowVersion = false;
	OsdShowSettings = false;
	OsdshowPatches = false;
	OsdShowInputs = false;
	OsdShowVideoCapture = true;
	OsdShowInputRec = true;
	OsdShowTextureReplacements = false;

	HWDownloadMode = GSHardwareDownloadMode::Enabled;
	HWSpinGPUForReadbacks = false;
	HWSpinCPUForReadbacks = false;
	GPUPaletteConversion = false;
	AutoFlushSW = true;
	PreloadFrameWithGSData = false;
	Mipmap = true;
	HWMipmap = true;
	HWAccurateAlphaTest = false;
	HWAA1 = false;
	UseDebugBlend = false;
	HWROV = false;
	HWROVLogging = false;
	HWROVBarriersVK = false;

	ManualUserHacks = false;
	UserHacks_AlignSpriteX = false;
	UserHacks_AutoFlush = GSHWAutoFlushLevel::Disabled;
	UserHacks_Limit24BitDepth = GSLimit24BitDepth::Disabled;
	UserHacks_CPUFBConversion = false;
	UserHacks_ReadTCOnClose = false;
	UserHacks_DisableDepthSupport = false;
	UserHacks_DisablePartialInvalidation = false;
	UserHacks_DisableSafeFeatures = false;
	UserHacks_DisableRenderFixes = false;
	UserHacks_MergePPSprite = false;
	UserHacks_ForceEvenSpritePosition = false;
	UserHacks_BilinearHack = GSBilinearDirtyMode::Automatic;
	UserHacks_NativePaletteDraw = false;

	DumpReplaceableTextures = false;
	DumpReplaceableMipmaps = false;
	DumpTexturesWithFMVActive = false;
	DumpDirectTextures = true;
	DumpPaletteTextures = true;
	LoadTextureReplacements = false;
	LoadTextureReplacementsAsync = true;
	PrecacheTextureReplacements = false;

	EnableVideoCapture = true;
	EnableVideoCaptureParameters = false;
	EnableAudioCapture = true;
	EnableAudioCaptureParameters = false;
}

bool Pcsx2Config::GSOptions::ShouldDump(u64, int) const
{
	return false;
}

namespace EmuFolders {
std::string Cache = "sdmc:/switch/un5-nx/cache";
std::string Logs = "sdmc:/switch/un5-nx";
std::string Resources = "sdmc:/switch/un5-nx/resources";
}

bool FMVstarted = false;

GSRendererType GSGetCurrentRenderer()
{
	return GSRendererType::OGL;
}

bool GSIsHardwareRenderer()
{
	return true;
}

// The device is never recreated: one window, one configuration.
bool GSreopen(bool, bool, GSRendererType, std::optional<const Pcsx2Config::GSOptions*>)
{
	return false;
}

std::pair<u8, u8> GSGetRGBA8AlphaMinMax(const void* data, u32 width, u32 height, u32 stride)
{
	GSVector4i minc = GSVector4i::xffffffff();
	GSVector4i maxc = GSVector4i::zero();

	const u8* ptr = static_cast<const u8*>(data);
	if ((width % 4) == 0)
	{
		for (u32 r = 0; r < height; r++)
		{
			const u8* rptr = ptr;
			for (u32 c = 0; c < width; c += 4)
			{
				const GSVector4i v = GSVector4i::load<false>(rptr);
				rptr += sizeof(GSVector4i);
				minc = minc.min_u32(v);
				maxc = maxc.max_u32(v);
			}

			ptr += stride;
		}
	}
	else
	{
		const u32 aligned_width = Common::AlignDownPow2(width, 4);
		static constexpr const GSVector4i masks[3][2] = {
			{GSVector4i::cxpr(0xFFFFFFFF, 0, 0, 0), GSVector4i::cxpr(0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)},
			{GSVector4i::cxpr(0xFFFFFFFF, 0xFFFFFFFF, 0, 0), GSVector4i::cxpr(0, 0, 0xFFFFFFFF, 0xFFFFFFFF)},
			{GSVector4i::cxpr(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0), GSVector4i::cxpr(0, 0, 0, 0xFFFFFFFF)},
		};
		const u32 unaligned_pixels = width & 3;
		const GSVector4i last_mask_and = masks[unaligned_pixels - 1][0];
		const GSVector4i last_mask_or = masks[unaligned_pixels - 1][1];

		for (u32 r = 0; r < height; r++)
		{
			const u8* rptr = ptr;
			for (u32 c = 0; c < aligned_width; c += 4)
			{
				const GSVector4i v = GSVector4i::load<false>(rptr);
				rptr += sizeof(GSVector4i);
				minc = minc.min_u32(v);
				maxc = maxc.max_u32(v);
			}

			GSVector4i v;
			u32 vu;
			if (unaligned_pixels == 3)
			{
				v = GSVector4i::loadl(rptr);
				std::memcpy(&vu, rptr + sizeof(u32) * 2, sizeof(vu));
				v = v.insert32<2>(vu);
			}
			else if (unaligned_pixels == 2)
			{
				v = GSVector4i::loadl(rptr);
			}
			else
			{
				std::memcpy(&vu, rptr, sizeof(vu));
				v = GSVector4i::load(vu);
			}

			minc = minc.min_u32(v | last_mask_or);
			maxc = maxc.max_u32(v & last_mask_and);

			ptr += stride;
		}
	}

	return std::make_pair<u8, u8>(static_cast<u8>(minc.minv_u32() >> 24),
		static_cast<u8>(maxc.maxv_u32() >> 24));
}

// ---- host --------------------------------------------------------------

std::optional<WindowInfo> Host::AcquireRenderWindow(bool)
{
	// GLContextEGLSwitch always uses the default window; this only sets the size.
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.surface_width = 1280;
	wi.surface_height = 720;
	wi.surface_scale = 1.0f;
	return wi;
}

void Host::BeginPresentFrame() {}

void Host::AddOSDMessage(std::string message, float)
{
	Console.WriteLn("[gs] %s", message.c_str());
}

void Host::AddKeyedOSDMessage(std::string, std::string message, float)
{
	Console.WriteLn("[gs] %s", message.c_str());
}

void Host::AddIconOSDMessage(std::string, const char*, const std::string_view message, float)
{
	Console.WriteLn("[gs] %.*s", static_cast<int>(message.size()), message.data());
}

bool Host::GetBoolSettingValue(const char*, const char*, bool default_value)
{
	return default_value;
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	Console.Error("[gs] %.*s: %.*s", static_cast<int>(title.size()), title.data(), static_cast<int>(message.size()), message.data());
}

void Host::ReportFormattedErrorAsync(const std::string_view title, const char* format, ...)
{
	char message[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	ReportErrorAsync(title, message);
}

std::string_view Host::TranslateToStringView(const std::string_view, const std::string_view msg)
{
	return msg;
}

// ---- PCSX2 subsystems the game does not use ----------------------------

void PerformanceMetrics::Update(bool, bool, bool) {}
void PerformanceMetrics::OnGPUPresent(float, u64, u64) {}
PerformanceMetrics::InternalFPSMethod PerformanceMetrics::GetInternalFPSMethod()
{
	return InternalFPSMethod::None;
}
bool PerformanceMetrics::IsSavingMetrics()
{
	return false;
}
void PerformanceMetrics::DumpSavedMetrics() {}

u32 VMManager::GetDiscCRC()
{
	return 0;
}
std::string VMManager::GetDiscSerial()
{
	return "SLES-55605";
}

bool GSCapture::DeliverVideoFrame(GSTexture*)
{
	return false;
}
void GSCapture::EndCapture() {}
bool GSCapture::IsCapturingVideo()
{
	return false;
}
GSVector2i GSCapture::GetSize()
{
	return GSVector2i(0, 0);
}

bool GSDumpReplayer::IsReplayingDump()
{
	return false;
}
bool GSDumpReplayer::IsRunner()
{
	return false;
}
void GSDumpReplayer::RenderUI() {}
void GSDumpReplayer::UpdateGSStats() {}

void GSTextureReplacements::Initialize() {}
bool GSTextureReplacements::HasAnyReplacementTextures()
{
	return false;
}
bool GSTextureReplacements::HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey&)
{
	return false;
}
GSTexture* GSTextureReplacements::LookupReplacementTexture(const GSTextureCache::HashCacheKey&, bool, bool*, std::pair<u8, u8>*)
{
	return nullptr;
}
void GSTextureReplacements::ProcessAsyncLoadedTextures() {}

bool GSPng::Save(GSPng::Format, const std::string&, const u8*, int, int, int, int, bool)
{
	return false;
}

RGBA8Image::RGBA8Image() = default;
RGBA8Image::RGBA8Image(RGBA8Image&& move) = default;
bool RGBA8Image::SaveToFile(const char*, u8) const
{
	return false;
}

const ImFontLoader* ImGuiFreeType::GetFontLoader()
{
	return nullptr;
}

// ---- files and paths ---------------------------------------------------

std::FILE* FileSystem::OpenCFile(const char* filename, const char* mode, Error*)
{
	return std::fopen(filename, mode);
}

FileSystem::ManagedCFilePtr FileSystem::OpenManagedCFile(const char* filename, const char* mode, Error* error)
{
	return ManagedCFilePtr(OpenCFile(filename, mode, error));
}

bool FileSystem::FileExists(const char* path)
{
	std::FILE* fp = std::fopen(path, "rb");
	if (!fp)
		return false;
	std::fclose(fp);
	return true;
}

bool FileSystem::DeleteFilePath(const char* path, Error*)
{
	return std::remove(path) == 0;
}

std::optional<std::string> FileSystem::ReadFileToString(const char* filename)
{
	std::FILE* fp = std::fopen(filename, "rb");
	if (!fp)
		return std::nullopt;
	std::string out;
	char buffer[16384];
	for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), fp)) > 0;)
		out.append(buffer, n);
	std::fclose(fp);
	return out;
}

std::string Path::Combine(const std::string_view base, const std::string_view next)
{
	std::string out(base);
	if (!out.empty() && out.back() != '/')
		out.push_back('/');
	out.append(next.starts_with('/') ? next.substr(1) : next);
	return out;
}

std::string_view Path::GetFileName(const std::string_view path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// ---- platform ----------------------------------------------------------

[[noreturn]] void AbortWithMessage(const char* msg)
{
	std::fprintf(stderr, "%s\n", msg);
	std::abort();
}

extern "C" int posix_memalign(void** out, size_t alignment, size_t size)
{
	*out = memalign(alignment, size);
	return *out ? 0 : ENOMEM;
}

const char* Pcsx2Config::GSOptions::DEFAULT_CAPTURE_CONTAINER = "mp4";

void GSDumpBase::Transfer(int, const u8*, size_t) {}
void GSDumpBase::ReadFIFO(u32) {}
bool GSDumpBase::VSync(int, bool, const GSPrivRegSet*)
{
	return false;
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateUncompressedDump(const std::string&, const std::string&, u32, u32, u32, const u32*, const freezeData&, const GSPrivRegSet*)
{
	return nullptr;
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateXzDump(const std::string&, const std::string&, u32, u32, u32, const u32*, const freezeData&, const GSPrivRegSet*)
{
	return nullptr;
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateZstDump(const std::string&, const std::string&, u32, u32, u32, const u32*, const freezeData&, const GSPrivRegSet*)
{
	return nullptr;
}

void GSTextureReplacements::DumpTexture(const GSTextureCache::HashCacheKey&, const GIFRegTEX0&, const GIFRegTEXA&, GSTextureCache::SourceRegion, GSLocalMemory&, u32) {}
