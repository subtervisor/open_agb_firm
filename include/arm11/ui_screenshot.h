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

// Boot-UI screenshot helper. Captures the contents of the top + bottom LCD
// framebuffers into a single 400x480 24-bit BMP under
// `${OAF_WORK_DIR}/${OAF_SCREENSHOT_DIR}/<timestamp>.bmp`.
//
// Independent from the in-game screenshot path in oaf_video.c which dumps the
// raw GBA-mode frame; this one captures whatever imgui rendered.

#include "types.h"
#include "oaf_error_codes.h"
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif


// `top_lcd_bgr` and `bottom_lcd_bgr` are pointers into the LCD framebuffers
// as returned by GFX_getBuffer (raw 240xN BGR8 columns, native LCD layout).
// `path_out` receives the chosen output path on success; sized for use in a
// status message.
Result oafBootUiScreenshot(const void *top_lcd_bgr,
                           const void *bottom_lcd_bgr,
                           char *path_out, size_t path_out_size);


#ifdef __cplusplus
} // extern "C"
#endif
