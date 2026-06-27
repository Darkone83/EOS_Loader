// eos_font.h -- AA proportional font rendering for the EOS Loader.
#pragma once
#include "eos_gfx.h"

bool  Font_Init();      // builds the font atlas texture from baked data
void  Font_Shutdown();

// Draw a string at (x,y) in 8x16 cells, tinted 'color'. Returns advance width.
int   Font_Draw(int x, int y, const char* s, DWORD color);

// Draw centered within [x0, x0+width]. Returns the x it started at.
int   Font_DrawCentered(int x0, int width, int y, const char* s, DWORD color);

int   Font_TextWidth(const char* s);   // pixels

#define FONT_CW 11   // representative advance; prefer Font_TextWidth
#define FONT_CH 22   // line height