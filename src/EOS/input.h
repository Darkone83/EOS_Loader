#pragma once
#include <xtl.h>

// Unified digital mask used throughout the loader.
// D-pad/Start/Back/thumb clicks use native XINPUT bits; the remaining buttons
// are synthesized from the Xbox analog-button array (with third-party digital
// wButtons fallbacks handled in input.cpp).
enum
{
    BTN_DPAD_UP = XINPUT_GAMEPAD_DPAD_UP,
    BTN_DPAD_DOWN = XINPUT_GAMEPAD_DPAD_DOWN,
    BTN_DPAD_LEFT = XINPUT_GAMEPAD_DPAD_LEFT,
    BTN_DPAD_RIGHT = XINPUT_GAMEPAD_DPAD_RIGHT,

    BTN_START = XINPUT_GAMEPAD_START,
    BTN_BACK = XINPUT_GAMEPAD_BACK,
    BTN_LTHUMB = XINPUT_GAMEPAD_LEFT_THUMB,
    BTN_RTHUMB = XINPUT_GAMEPAD_RIGHT_THUMB,

    BTN_A = 0x1000,
    BTN_B = 0x2000,
    BTN_X = 0x4000,
    BTN_Y = 0x8000,
    BTN_BLACK = 0x0100,
    BTN_WHITE = 0x0200,
    BTN_LTRIG = 0x0400,
    BTN_RTRIG = 0x0800,
};

// Register and poll gamepads on all four Xbox controller ports.
void InitInput();
void PumpInput();

// OR of the synthesized button state from every connected controller, so the
// loader can always be operated from any controller port.
WORD GetButtons();
