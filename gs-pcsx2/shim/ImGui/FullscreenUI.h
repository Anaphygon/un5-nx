// See ImGuiManager.h: no fullscreen UI.
#pragma once
namespace FullscreenUI {
inline void Render() {}
inline float GetBackgroundBlurScale() { return 1.0f; }
}
