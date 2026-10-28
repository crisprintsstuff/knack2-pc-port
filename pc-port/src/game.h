// SPDX-License-Identifier: GPL-2.0-or-later
// The one game this runtime targets. Saves, trophies and the GPU layer key their paths on it.
#pragma once
#define GAME_TITLE_ID "CUSA08014"
#define GAME_TITLE "Knack 2"

// Knack II is a two-stage boot: this runtime's first image is a 37 KB loader stub whose only job is
// to load /app0/game.elf. The stub's entry is +0x4fa0 and the game's own entry is +0x80, so the
// launcher has to run the second image's entry point, not the first one's.
#define GAME_STUB_ENTRY 0x4fa0
#define GAME_MAIN_ENTRY 0x80
#define GAME_MAIN_PATH "/app0/game.elf"
