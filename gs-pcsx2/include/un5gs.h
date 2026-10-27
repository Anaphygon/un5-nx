// SPDX-License-Identifier: GPL-3.0+
// un5-nx's interface to PCSX2's GS. Plain types only, so the caller (the
// runtime's GIF and privileged-register layer) needs no PCSX2 header.
#pragma once

#include <cstdint>

namespace un5gs {

// Creates the OpenGL device on the Switch's window and the hardware renderer.
bool open();
void close();

// GIF packets as the GIF unit hands them over: path 1 (VU1 XGKICK), path 2
// (VIF1 DIRECT) or path 3 (GIF DMA), in quadwords.
void transfer(int path, const void* data, std::uint32_t qwords);

// The privileged registers (PMODE, SMODE2, DISPFB, DISPLAY, CSR, ...): 8 KiB
// laid out as the PS2 maps them at 0x12000000, written by the runtime.
std::uint8_t* privileged_registers();
void write_csr(std::uint32_t csr);

// Ends a field: draws pending work and presents the frame.
void vsync(std::uint32_t field, bool registers_written);

// Reads GS memory back for a GS-to-host transfer (TRXDIR = 1).
void read_fifo(void* data, std::uint32_t qwords);

}
