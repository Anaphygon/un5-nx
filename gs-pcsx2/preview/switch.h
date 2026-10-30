/* Just enough of libnx for the launcher (launcher.cpp, launcher_gfx.cpp) to
 * build on a desktop, so its screen can be looked at without a console.
 * Adapted from switch-devtools' preview/switch.h (MIT). */
#ifndef UN5_PREVIEW_SWITCH_H
#define UN5_PREVIEW_SWITCH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int16_t s16; typedef int32_t s32; typedef int64_t s64; typedef u32 Result;
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)

typedef struct { u64 down, held; } PadState;
enum { HidNpadButton_A = 1 << 0, HidNpadButton_B = 1 << 1, HidNpadButton_X = 1 << 2, HidNpadButton_Y = 1 << 3,
       HidNpadButton_Plus = 1 << 10, HidNpadButton_Minus = 1 << 11,
       HidNpadButton_AnyLeft = 1 << 12, HidNpadButton_AnyUp = 1 << 13,
       HidNpadButton_AnyRight = 1 << 14, HidNpadButton_AnyDown = 1 << 15 };
#define HidNpadStyleSet_NpadStandard 0
void padConfigureInput(u32 players, u32 styles);
void padInitializeAny(PadState *p);
void padUpdate(PadState *p);
static inline u64 padGetButtonsDown(PadState *p) { return p->down; }
static inline u64 padGetButtons(PadState *p) { return p->held; }
bool appletMainLoop(void);
void svcSleepThread(s64 ns);

typedef struct { int has_init; } Framebuffer;
typedef struct { int unused; } NWindow;
#define PIXEL_FORMAT_RGBA_8888 1
NWindow *nwindowGetDefault(void);
Result framebufferCreate(Framebuffer *fb, NWindow *w, u32 width, u32 height, u32 fmt, u32 n);
Result framebufferMakeLinear(Framebuffer *fb);
void *framebufferBegin(Framebuffer *fb, u32 *stride);
void framebufferEnd(Framebuffer *fb);
void framebufferClose(Framebuffer *fb);

typedef enum { PlServiceType_User } PlServiceType;
typedef enum { PlSharedFontType_Standard, PlSharedFontType_NintendoExt = 5 } PlSharedFontType;
typedef struct { u32 type, offset, size; void *address; } PlFontData;
Result plInitialize(PlServiceType t);
Result plGetSharedFontByType(PlFontData *font, PlSharedFontType t);
void plExit(void);
#endif
