// SPDX-License-Identifier: GPL-3.0+
// common/DynamicLibrary for the Switch, which has no dlopen: the only library
// PCSX2's GS opens is libEGL, which is linked in statically, so every symbol
// is looked up through eglGetProcAddress (Mesa answers for core EGL too).
#include "common/DynamicLibrary.h"
#include "common/Error.h"

#include <EGL/egl.h>

#include <utility>

namespace {
// Any non-null handle: there is one library, already loaded.
char s_linked;
}

DynamicLibrary::DynamicLibrary() = default;

DynamicLibrary::DynamicLibrary(const char* filename)
{
	Open(filename, nullptr);
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& move)
	: m_handle(move.m_handle)
{
	move.m_handle = nullptr;
}

DynamicLibrary::~DynamicLibrary()
{
	Close();
}

std::string DynamicLibrary::GetUnprefixedFilename(const char* filename)
{
	return filename;
}

std::string DynamicLibrary::GetVersionedFilename(const char* libname, int, int)
{
	return libname;
}

bool DynamicLibrary::Open(const char*, Error*)
{
	m_handle = &s_linked;
	return true;
}

void DynamicLibrary::Adopt(void* handle)
{
	m_handle = handle;
}

void DynamicLibrary::Close()
{
	m_handle = nullptr;
}

void* DynamicLibrary::GetSymbolAddress(const char* name) const
{
	return reinterpret_cast<void*>(eglGetProcAddress(name));
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& move)
{
	m_handle = std::exchange(move.m_handle, nullptr);
	return *this;
}
