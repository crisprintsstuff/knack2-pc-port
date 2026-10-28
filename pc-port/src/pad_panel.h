// SPDX-License-Identifier: GPL-2.0-or-later
// The touchpad window (F5 in game, src/pad_panel.c), read by src/pad.c.
#pragma once
#include <windows.h>
typedef struct {
    int touch,two,click;float x,y; // a finger on the window's pad (0..1); Shift: two; right button: click
    int left,right,both,centre;    // buttons held down
    int swipe,mode,recentre;       // once: swipe 0 up 1 down 2 left 3 right (-1 none), mode 1/2 (-1 none)
    float sensitivity;             // the spray can's speed (1 = 100%)
} PadPanel;
void pad_panel_toggle(HWND game);   // shows or hides the window, next to the game window
void pad_panel_read(PadPanel *out); // the window's input since the last read
void pad_panel_set_mode(int mode);  // the mouse mode pad.c is in (0 off, 1 touchpad, 2 spray)
int pad_panel_raw(float *dx,float *dy); // the mouse's raw counts since the last call; 0 while Raw Input is not delivering
HWND pad_panel_window(void);
