// SPDX-License-Identifier: GPL-2.0-or-later
// The save data dialog shared by iss-probe.exe (src/save_dialog.c: the sceSaveDataDialog ABI the
// game calls) and iss-gpu.dll (gpu/save_dialog_ui.cpp: drawn over the game with ImGui, driven by
// the controller, the keyboard and the mouse). Plain C: both sides include it.
#pragma once
#include <stdint.h>

#define ISS_DIALOG_MAX_ITEMS 64

enum IssDialogMode { IssDialogList = 1, IssDialogUserMessage = 2, IssDialogSystemMessage = 3,
                     IssDialogErrorCode = 4, IssDialogProgress = 5 };
enum IssDialogType { IssDialogSave = 1, IssDialogLoad = 2, IssDialogDelete = 3 };
enum IssDialogButtons { IssDialogOk = 0, IssDialogYesNo = 1, IssDialogNone = 2 };

typedef struct IssDialogItem {
    char dir[32];            // save directory; empty for the "new saved data" entry
    char title[128];         // UTF-8
    char subtitle[128];
    char detail[256];
    int64_t mtime;           // seconds since 1970 (0: unknown)
    char icon_path[520];     // UTF-8 path of a PNG icon (empty: none)
    const void* icon_png;    // or a PNG in memory (copied when the dialog opens)
    uint32_t icon_png_size;
    uint32_t reserved;
} IssDialogItem;

typedef struct IssDialog {
    uint32_t mode;           // IssDialogMode
    uint32_t type;           // IssDialogType
    uint32_t buttons;        // IssDialogButtons (messages)
    uint32_t count;          // list items
    uint32_t focus;          // initially focused item
    uint32_t default_no;     // yes/no: focus starts on No
    char text[1024];         // message (UTF-8)
    IssDialogItem items[ISS_DIALOG_MAX_ITEMS];
} IssDialog;

typedef struct IssDialogResult {
    int32_t canceled;        // closed with Back / Circle
    int32_t index;           // chosen list item
    int32_t button;          // messages: 1 OK / Yes, 2 No
    int32_t reserved;
} IssDialogResult;

// iss-gpu.dll exports (looked up by src/gpu.c):
//   int  iss_gpu_dialog_open(const IssDialog*)     0 on success
//   int  iss_gpu_dialog_poll(IssDialogResult*)     1 finished (result filled), 0 open, -1 none
//   void iss_gpu_dialog_close(void)                closes it without a result
//   int  iss_gpu_dialog_active(void)               1 while it is on screen (the game's pad is idle)
