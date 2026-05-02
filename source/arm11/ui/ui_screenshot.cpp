/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2024 derrek, profi200
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Adapted from oaf-configurator-firm-imgui/source/screenshot.cpp.
 */

#include "arm11/ui_screenshot.h"
#include "arm11/config.h"  // OAF_WORK_DIR / OAF_SCREENSHOT_DIR

#include <cstring>

extern "C"
{
#include <arm11/allocator/fcram.h>
#include <arm11/drivers/mcu.h>
#include <arm11/fmt.h>
#include <drivers/gfx.h>
#include <fs.h>
}


namespace
{

constexpr u32 TOP_WIDTH    = 400;
constexpr u32 TOP_HEIGHT   = 240;
constexpr u32 BOTTOM_WIDTH = 320;
constexpr u32 BOTTOM_HEIGHT = 240;
constexpr u32 LCD_FB_WIDTH  = 240; // 3DS framebuffers store columns; height in pixels = 240.
constexpr u32 BMP_WIDTH    = TOP_WIDTH;
constexpr u32 BMP_HEIGHT   = TOP_HEIGHT + BOTTOM_HEIGHT;
constexpr u32 BOTTOM_X     = (BMP_WIDTH - BOTTOM_WIDTH) / 2;
constexpr u32 BYTES_PER_PIXEL = 3;
constexpr u32 BMP_HEADER_SIZE = 14 + 40;
constexpr u32 BMP_ROW_SIZE    = (BMP_WIDTH * BYTES_PER_PIXEL + 3) & ~3u;
constexpr u32 BMP_IMAGE_SIZE  = BMP_ROW_SIZE * BMP_HEIGHT;
constexpr u32 BMP_FILE_SIZE   = BMP_HEADER_SIZE + BMP_IMAGE_SIZE;


struct BmpFileHeader
{
	u16 magic;
	u32 file_size;
	u16 reserved0;
	u16 reserved1;
	u32 pixel_offset;
} PACKED;
static_assert(sizeof(BmpFileHeader) == 14);

struct BmpInfoHeader
{
	u32 header_size;
	s32 width;
	s32 height;
	u16 color_planes;
	u16 bits_per_pixel;
	u32 compression;
	u32 image_size;
	s32 x_pixels_per_meter;
	s32 y_pixels_per_meter;
	u32 colors_used;
	u32 colors_important;
} PACKED;
static_assert(sizeof(BmpInfoHeader) == 40);

struct BmpHeader
{
	BmpFileHeader file;
	BmpInfoHeader info;
} PACKED;
static_assert(sizeof(BmpHeader) == BMP_HEADER_SIZE);


Result mkdirIfNeeded(const char *path)
{
	const Result r = fMkdir(path);
	if(r == RES_OK || r == RES_FR_EXIST) return RES_OK;
	return r;
}

Result makeScreenshotPath(char *path, size_t path_size)
{
	if(path == NULL || path_size == 0) return RES_INVALID_ARG;

	RtcTimeDate td{};
	MCU_getRtcTimeDate(&td);

	const u32 written = ee_snprintf(
	    path, path_size,
	    OAF_SCREENSHOT_DIR "/%04X_%02X_%02X_%02X_%02X_%02X.bmp",
	    static_cast<unsigned>(td.year) + 0x2000u,
	    static_cast<unsigned>(td.mon),
	    static_cast<unsigned>(td.day),
	    static_cast<unsigned>(td.hour),
	    static_cast<unsigned>(td.min),
	    static_cast<unsigned>(td.sec));
	if(written == 0 || written >= path_size) return RES_OUT_OF_RANGE;
	return RES_OK;
}

// Walk the LCD framebuffer (which is rotated 90° relative to the BMP layout)
// and write straightened BGR rows.
void copyLcdToBmp(u8 *bmp_pixels, const u8 *lcd_bgr,
                  u32 src_width, u32 src_height,
                  u32 dst_x, u32 dst_y)
{
	for(u32 y = 0; y < src_height; ++y)
	{
		u8 *const dst_row = bmp_pixels + (dst_y + y) * BMP_ROW_SIZE + dst_x * BYTES_PER_PIXEL;
		for(u32 x = 0; x < src_width; ++x)
		{
			const u8 *const src = lcd_bgr + (x * LCD_FB_WIDTH + (LCD_FB_WIDTH - 1 - y)) * BYTES_PER_PIXEL;
			u8 *const dst = dst_row + x * BYTES_PER_PIXEL;
			std::memcpy(dst, src, BYTES_PER_PIXEL);
		}
	}
}

Result writeBmp(const char *path, const void *data, u32 size)
{
	FHandle f;
	Result r = fOpen(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
	if(r != RES_OK) return r;

	u32 bytes_written = 0;
	r = fWrite(f, data, size, &bytes_written);
	fClose(f);

	if(r != RES_OK) return r;
	if(bytes_written != size) return RES_OUT_OF_RANGE;
	return RES_OK;
}

} // namespace


extern "C" Result oafBootUiScreenshot(const void *top_lcd_bgr,
                                      const void *bottom_lcd_bgr,
                                      char *path_out, size_t path_out_size)
{
	if(top_lcd_bgr == NULL || bottom_lcd_bgr == NULL || path_out == NULL)
		return RES_INVALID_ARG;

	Result r = mkdirIfNeeded(OAF_SCREENSHOT_DIR);
	if(r != RES_OK) return r;

	r = makeScreenshotPath(path_out, path_out_size);
	if(r != RES_OK) return r;

	u8 *const bmp = static_cast<u8*>(fcramAlloc(BMP_FILE_SIZE));
	if(bmp == NULL) return RES_OUT_OF_MEM;

	const BmpHeader header = {
	    {
	        /*magic*/        0x4D42,
	        /*file_size*/    BMP_FILE_SIZE,
	        /*reserved0*/    0,
	        /*reserved1*/    0,
	        /*pixel_offset*/ BMP_HEADER_SIZE,
	    },
	    {
	        /*header_size*/        sizeof(BmpInfoHeader),
	        /*width*/              static_cast<s32>(BMP_WIDTH),
	        /*height*/             -static_cast<s32>(BMP_HEIGHT),
	        /*color_planes*/       1,
	        /*bits_per_pixel*/     24,
	        /*compression*/        0,
	        /*image_size*/         BMP_IMAGE_SIZE,
	        /*x_pixels_per_meter*/ 0,
	        /*y_pixels_per_meter*/ 0,
	        /*colors_used*/        0,
	        /*colors_important*/   0,
	    },
	};

	std::memcpy(bmp, &header, sizeof(header));
	u8 *const pixels = bmp + BMP_HEADER_SIZE;
	std::memset(pixels, 0, BMP_IMAGE_SIZE);

	copyLcdToBmp(pixels, static_cast<const u8*>(top_lcd_bgr),
	             TOP_WIDTH, TOP_HEIGHT, 0, 0);
	copyLcdToBmp(pixels, static_cast<const u8*>(bottom_lcd_bgr),
	             BOTTOM_WIDTH, BOTTOM_HEIGHT, BOTTOM_X, TOP_HEIGHT);

	r = writeBmp(path_out, bmp, BMP_FILE_SIZE);
	fcramFree(bmp);
	return r;
}
