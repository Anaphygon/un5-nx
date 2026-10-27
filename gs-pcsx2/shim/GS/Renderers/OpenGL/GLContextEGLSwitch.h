// SPDX-License-Identifier: GPL-3.0+
// An EGL context on the Switch's default window (libnx nwindow), for PCSX2's
// OpenGL device. Mesa on the Switch has no platform extensions, so this asks
// for the default display and makes a plain window surface.
#pragma once

#include "GS/Renderers/OpenGL/GLContextEGL.h"

class GLContextEGLSwitch final : public GLContextEGL
{
public:
	GLContextEGLSwitch(const WindowInfo& wi);

	static std::unique_ptr<GLContext> Create(const WindowInfo& wi, std::span<const Version> versions_to_try, Error* error);

	std::unique_ptr<GLContext> CreateSharedContext(const WindowInfo& wi, Error* error) override;

protected:
	EGLDisplay GetPlatformDisplay(Error* error) override;
	EGLSurface CreatePlatformSurface(EGLConfig config, void* win, Error* error) override;
};
