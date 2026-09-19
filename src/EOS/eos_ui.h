// eos_ui.h -- shared menu chrome so every screen uses identical pill styling.
//
// One home for the title pill, footer hint, and the pill rows (centered,
// left-aligned, and label/value). Geometry is caller-supplied so each screen
// can size its list, but the colors/shape are uniform.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#pragma once
#include <xtl.h>

void Ui_Backdrop(void);
void Ui_TitleBar(const char* title);                 // centered accent title pill
void Ui_Footer(const char* hint);                    // centered dim hint, bottom-safe
void Ui_StatusToast(const char* text);              // transient global status pill
void Ui_ScrollBar(int x, int y, int h, int first, int visible, int total);

// Fit helpers shared with the updater: normal size when possible, scale only
// when text would exceed the supplied region.
void Ui_TextCenteredFit(int x0, int width, int y, const char* text, DWORD color);
void Ui_TextLeftFit(int x, int y, int maxW, const char* text, DWORD color);

// Lightweight phase veil: new screens appear under a short fading dark layer
// with a themed scan edge. No textures/assets and no blocking delays.
void Ui_TransitionStart(void);
int  Ui_TransitionActive(void);
void Ui_TransitionDraw(void);

// Selectable pills. selected -> accent fill + white text; otherwise a raised
// dark surface + dim text. dim=1 forces muted label even when not selected
// (read-only rows).
void Ui_PillCentered(int x, int y, int w, int h, int r, int selected, const char* label);
void Ui_PillLeft(int x, int y, int w, int h, int r, int selected, const char* label);
void Ui_PillRow(int x, int y, int w, int h, int r, int selected, int dim,
    const char* label, const char* value);

// Shared 3D menu: geometric capsules on a rail receding into the screen.
// Long labels scale to fit the same capped pill width used by the updater, and
// selected glow/pulse geometry is identical between both applications.
void Ui_Menu3D(const char** items, int count, int sel);
// Same wheel with explicit screen-space safe bounds. Receding items fade out
// before crossing topY/bottomY; the selected pill is never altered.
void Ui_Menu3DBounded(const char** items, int count, int sel, int topY, int bottomY);

// Shared metrics so screens line up.
#define UI_PILL_H   38
#define UI_PILL_R   19
#define UI_ROW_DY   46
#define UI_PILL_BG  EOS_PANEL   /* raised themed panel surface */