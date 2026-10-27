// The game draws no PCSX2 menus or on-screen display: GS's calls do nothing.
#pragma once
namespace ImGuiManager {
inline bool Initialize() { return true; }
inline void Shutdown(bool = false) {}
inline void NewFrame() {}
inline void SkipFrame() {}
inline void RenderOSD() {}
inline void ReloadFonts() {}
inline void RequestScaleUpdate() {}
inline void WindowResized(float = 0.0f, float = 0.0f) {}
}
