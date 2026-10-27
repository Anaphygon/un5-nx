// SPDX-License-Identifier: GPL-3.0+
#include "GS/Renderers/OpenGL/GLContextEGLSwitch.h"

#include "common/Error.h"

// libnx's <switch.h> defines u128/s128 differently from PCSX2's headers, so
// the one function needed from it is declared here.
extern "C" struct NWindow* nwindowGetDefault(void);

GLContextEGLSwitch::GLContextEGLSwitch(const WindowInfo& wi)
	: GLContextEGL(wi)
{
}

std::unique_ptr<GLContext> GLContextEGLSwitch::Create(const WindowInfo& wi, std::span<const Version> versions_to_try, Error* error)
{
	std::unique_ptr<GLContextEGLSwitch> context = std::make_unique<GLContextEGLSwitch>(wi);
	if (!context->Initialize(versions_to_try, error))
		return nullptr;
	return context;
}

std::unique_ptr<GLContext> GLContextEGLSwitch::CreateSharedContext(const WindowInfo& wi, Error* error)
{
	std::unique_ptr<GLContextEGLSwitch> context = std::make_unique<GLContextEGLSwitch>(wi);
	context->m_display = m_display;
	if (!context->CreateContextAndSurface(m_version, m_context, false))
		return nullptr;
	return context;
}

EGLDisplay GLContextEGLSwitch::GetPlatformDisplay(Error* error)
{
	const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY)
		Error::SetStringView(error, "eglGetDisplay(EGL_DEFAULT_DISPLAY) failed");
	return display;
}

EGLSurface GLContextEGLSwitch::CreatePlatformSurface(EGLConfig config, void*, Error* error)
{
	// The surface is always the application's default window, whatever the WindowInfo says.
	const EGLSurface surface = eglCreateWindowSurface(m_display, config, nwindowGetDefault(), nullptr);
	if (surface == EGL_NO_SURFACE)
		Error::SetStringFmt(error, "eglCreateWindowSurface() failed: 0x{:X}", static_cast<unsigned>(eglGetError()));
	return surface;
}
