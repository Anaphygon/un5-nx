# gs-pcsx2

PCSX2's GS (hardware renderer, OpenGL device) built as a library for the
Switch, to replace ps2-web-recomp's compute-shader GS there. PCSX2 is
GPL-3.0, as un5-nx is; nothing here goes into the MIT ps2-web-recomp patches.

- Source: a sparse checkout of PCSX2 (`pcsx2/GS`, `common`, `3rdparty/{fmt,glad,imgui,include,fast_float}`,
  `bin/resources/shaders/{opengl,common}`) at `PCSX2_SRC`, with `patches/` applied.
- `include/un5gs.h`: the plain interface the runtime calls (GIF transfers, privileged registers, vsync).
- `src/gs_front.cpp`: our front end in place of PCSX2's `GS.cpp`.
- `src/pcsx2_glue.cpp`: what the GS expects from the rest of PCSX2, as defaults or nothing.
- `src/GLContextEGLSwitch.cpp`, `DynamicLibrarySwitch.cpp`, `wrapped_memory_switch.cpp`, `switch_time.cpp`: the Switch platform layer.
- `shim/`: stand-ins for cpuinfo and PCSX2's ImGui UI.

PCSX2's rules (its AGENTS.md) forbid AI agents from opening pull requests,
issues or comments there and new contributors from sending LLM-written code:
nothing from here is to be sent upstream by an agent.

Status: runs on a Switch (HOS 23.0.0): UN5 intro, battles and Master Mode, below full speed.
