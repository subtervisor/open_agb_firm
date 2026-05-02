#pragma once
/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2021 derrek, profi200
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 */

// Pure directory-listing helper. Originally lived inside filebrowser.c
// alongside the console-text picker UI; the imgui file browser
// (ui_filebrowser.cpp) now owns the rendering side and uses this for the
// scan/sort logic.

#include "types.h"
#include "oaf_error_codes.h"


#ifdef __cplusplus
extern "C" {
#endif


// Same caps as the original filebrowser.c — sized to a typical SD layout.
#define FB_MAX_ENT_BUF_SIZE  (1024u * 196) // 196 KiB
#define FB_MAX_DIR_ENTRIES   (1000u)

#define FB_ENT_TYPE_FILE  (0)
#define FB_ENT_TYPE_DIR   (1)


typedef struct
{
	u32   num;                          // Total entries.
	char  entBuf[FB_MAX_ENT_BUF_SIZE];  // Format: char entryType; char name[X]; (NUL-terminated).
	char *ptrs[FB_MAX_DIR_ENTRIES];     // For fast sorting.
} FbDirList;


// Read directory entries at `path`, filter to those ending in `filter`
// (e.g. ".gba"; pass "" for all files), populate `dList`, and sort the
// result alphabetically with directories first. Returns RES_OK on success.
Result fbScanDir(const char *path, FbDirList *dList, const char *filter);


#ifdef __cplusplus
} // extern "C"
#endif
