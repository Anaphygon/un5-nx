// SPDX-License-Identifier: GPL-3.0+
// Hands ps2-web-recomp's runner PCSX2's GS (gs-pcsx2) as its external GS.
// Compiled into ps2run through PS2_EXTRA_SOURCES, as an object rather than
// from a library, so the linker keeps the registration below.
#include "ps2/external_gs.hpp"
#include "ps2/system.hpp"

#include <cstdlib>

#ifdef __SWITCH__
bool un5_launcher(const char* nro_dir);
#endif

#include "../include/un5gs.h"

namespace {

bool open()
{
	return un5gs::open();
}

void transfer(int path, const void* data, std::uint32_t qwc)
{
	un5gs::transfer(path, data, qwc);
}

std::uint8_t* privileged()
{
	return un5gs::privileged_registers();
}

void vsync(std::uint32_t field)
{
	un5gs::vsync(field, true);
}

const ps2::ExternalGs kPcsx2Gs{open, transfer, privileged, un5gs::write_csr, vsync, un5gs::read_fifo, un5gs::close};

#ifndef __SWITCH__
// The Mac runner links it for the shader harvest only: PS2_EXTERNAL_GS=1 asks
// for it, so its regression runs keep the runtime's own GS.
[[maybe_unused]] const bool kRegistered = std::getenv("PS2_EXTERNAL_GS") && (ps2::set_external_gs(&kPcsx2Gs), true);
#else
[[maybe_unused]] const bool kRegistered = (ps2::set_external_gs(&kPcsx2Gs), true);
#endif

#ifdef __SWITCH__
// The settings screen before the game (gs-pcsx2/src/launcher.cpp).
[[maybe_unused]] const bool kLauncher = (ps2::set_port_launcher(un5_launcher), true);
#endif

}
