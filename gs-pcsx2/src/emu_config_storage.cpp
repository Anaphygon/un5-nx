// SPDX-License-Identifier: GPL-3.0+
// PCSX2's EmuConfig is a Pcsx2Config, whose constructors live with the rest
// of PCSX2's VM (memory cards, pads, USB). Of it the GS reads only the two
// aspect-ratio fields, which gs_front.cpp sets; the rest stays zeroed and must
// not be read. Defined here, out of sight of Config.h, as plain storage.
alignas(64) unsigned char EmuConfig[1 << 16];
