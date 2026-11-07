// SPDX-License-Identifier: GPL-3.0+
// Parts of PCSX2 the GS refers to but the shader harvest never reaches
// (captures, file dialogs, the VM's settings and title). The Switch build's
// linker drops the code that refers to them; the Mac's wants them defined.
// Also the OpenGL device's counters that the deko3d device reports into.
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Threading.h"
#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"
#include "GS/GSCapture.h"

double g_un5_swap_ms = 0.0, g_un5_program_ms = 0.0;
u64 g_un5_gpu_behind = 0, g_un5_presents = 0;

namespace EmuFolders {
std::string Snapshots;
std::string Videos;
}

bool FileSystem::DirectoryExists(const char*) { return false; }
bool FileSystem::CreateDirectoryPath(const char*, bool, Error*) { return false; }
FileSystem::ManagedCFilePtr FileSystem::OpenManagedSharedCFile(const char*, const char*, FileShareMode, Error*) { return nullptr; }
void Path::SanitizeFileName(std::string*, bool) {}
bool GSCapture::BeginCapture(float, GSVector2i, float, std::string) { return false; }
bool Host::GetBaseBoolSettingValue(const char*, const char*, bool default_value) { return default_value; }
std::string Host::GetStringSettingValue(const char*, const char*, const char* default_value) { return default_value; }
void PerformanceMetrics::StartSavingMetrics(u32) {}
void Threading::SleepUntil(u64) {}
std::string VMManager::GetTitle(bool) { return "UN5"; }
