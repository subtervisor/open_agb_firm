#pragma once
/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2024 derrek, profi200
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 */

// Boot-time imgui UI for open_agb_firm. Owns C3D init + render targets +
// the imgui context. Active from main()'s init until just before the GBA
// emulator is handed the GPU via LGY11_switchMode (oafBootUiShutdown).

#include "types.h"
#include "oaf_error_codes.h"


#ifdef __cplusplus
extern "C" {
#endif


typedef enum
{
	OAF_UI_RESULT_PICKED_ROM = 0, // outRomPath holds the chosen .gba path.
	OAF_UI_RESULT_EXIT       = 1, // User chose Exit from the main menu.
	OAF_UI_RESULT_ERROR      = 2, // Init failure or unrecoverable error.
} OafBootUiResult;


// One-shot init: C3D, render targets, imgui context, font, input. Must be
// called after GFX_init. Safe to call before SD-mounted resources exist.
Result oafBootUiInit(void);

// Tear down everything (imgui context, render targets, C3D). After this
// returns the GPU is in a clean state for LGY11_switchMode. Idempotent —
// safe to call even if init failed or was never called.
void oafBootUiShutdown(void);

// Run the boot-time main menu loop. Blocks until the user picks a ROM
// (returns OAF_UI_RESULT_PICKED_ROM with outRomPath populated), or chooses
// Exit (OAF_UI_RESULT_EXIT). Loops forever otherwise.
//
// outRomPath must be at least 512 bytes.
OafBootUiResult oafBootUiRunMenu(char outRomPath[512]);

// Open a modal-progress window. `title` shows in the window title bar.
// Multiple BeginProgress/EndProgress pairs may nest visually but only the
// topmost is rendered.
void oafBootUiBeginProgress(const char *title);

// Update the status line of the active progress window. Variadic so callers
// can use printf-style formatting via the custom imgui_format formatter.
void oafBootUiUpdateProgress(const char *statusFmt, ...);

// Close the active progress window.
void oafBootUiEndProgress(void);

// Show an error modal with the message for `res`, then block until the user
// presses A (or Power, which also exits the FIRM).
void oafBootUiShowError(Result res);

// Show a free-text modal — same shape as oafBootUiShowError but with a
// caller-supplied printf-formatted body. Used for warnings/info that don't
// map to a Result code (e.g. "ROM is too big" during ROM load). Blocks
// until A or Power.
void oafBootUiShowMessage(const char *fmt, ...);

// Show the save-type override modal. Replaces the old console-text picker
// in save_type.c when the boot UI is active.
//
// Inputs:
//   autoDetected — what detectSaveType() returned (raw save-type id 0..15).
//   dbType       — what gba_db.bin reported (only meaningful if dbFound).
//   dbFound      — true if the gba_db.bin lookup succeeded.
//   saveExists   — true if a save file exists on disk for this ROM.
//
// Outputs:
//   *outSaveType        — the user's chosen save-type id (0..15).
//   *outDeleteRequested — true if the user pressed Delete; caller is
//                          expected to fUnlink the save file.
void oafBootUiSaveTypeOverride(u16 autoDetected, u16 dbType, bool dbFound,
                               bool saveExists,
                               u16 *outSaveType, bool *outDeleteRequested);


#ifdef __cplusplus
} // extern "C"
#endif
