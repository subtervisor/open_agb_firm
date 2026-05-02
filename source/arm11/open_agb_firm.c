/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2024 derrek, profi200
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "util.h"
#include "arm11/fast_rom_padding.h"
#include "oaf_error_codes.h"
#include "fs.h"
#include "arm11/fmt.h"
#include "arm11/drivers/mcu.h"
#include "drivers/gfx.h"
#include "arm11/drivers/hid.h"
#include "fsutil.h"
#include "arm11/config.h"
#include "arm11/ui_main.h"
#include "arm11/save_type.h"
#include "arm11/patch.h"
#include "arm11/drivers/codec.h"
#include "drivers/lgy_common.h"
#include "arm11/oaf_video.h"
#include "arm11/drivers/lgy11.h"
#include "kernel.h"
#include "kevent.h"


static KHandle g_frameReadyEvent = 0;



static u32 fixRomPadding(const u32 romFileSize)
{
	// Pad unused ROM area with 0xFFs (trimmed ROMs).
	// Smallest retail ROM chip is 8 Mbit (1 MiB).
	u32 romSize = nextPow2(romFileSize);
	romSize = (romSize < 0x100000 ? 0x100000 : romSize);
	const uintptr_t romLoc = LGY_ROM_LOC;
	memset((void*)(romLoc + romFileSize), 0xFF, romSize - romFileSize);

	u32 mirroredSize = romSize;
	if(romSize == 0x100000) // 1 MiB.
	{
		// ROM mirroring for Classic NES Series/others with 8 Mbit ROM.
		// The ROM is mirrored exactly 4 times.
		// Thanks to endrift for discovering this.
		mirroredSize = 0x400000; // 4 MiB.
		uintptr_t mirrorLoc = romLoc + romSize;
		do
		{
			memcpy((void*)mirrorLoc, (void*)romLoc, romSize);
			mirrorLoc += romSize;
		} while(mirrorLoc < romLoc + mirroredSize);
	}

	// Fake "open bus" padding.
	if(romSize < LGY_MAX_ROM_SIZE)
		makeOpenBusPaddingFast((u32*)(romLoc + mirroredSize));

	// We don't return the mirrored size because the db hashes are over unmirrored dumps.
	return romSize;
}

static Result loadGbaRom(const char *const path, u32 *const romSizeOut)
{
	FHandle f;
	Result res = fOpen(&f, path, FA_OPEN_EXISTING | FA_READ);
	if(res == RES_OK)
	{
		u32 fileSize = fSize(f);
		if(fileSize > LGY_MAX_ROM_SIZE)
		{
			fileSize = LGY_MAX_ROM_SIZE;
			oafBootUiShowMessage("Warning: ROM file is too big (>32 MiB). "
			                     "Truncating to fit; expect crashes.");
		}

		u32 read;
		res = fRead(f, (void*)LGY_ROM_LOC, fileSize, &read);
		fClose(f);

		if(read == fileSize) *romSizeOut = fixRomPadding(fileSize);

	}

	return res;
}

void changeBacklight(s16 amount)
{
	u8 min, max;
	if(MCU_getSystemModel() >= 4)
	{
		min = 16;
		max = 142;
	}
	else
	{
		min = 20;
		max = 117;
	}

	s16 newVal = g_oafConfig.backlight + amount;
	newVal = (newVal > max ? max : newVal);
	newVal = (newVal < min ? min : newVal);
	g_oafConfig.backlight = (u8)newVal;

	GFX_setLcdLuminance(newVal);
}

static void updateBacklight(void)
{
	// Check for special button combos.
	const u32 kHeld = hidKeysHeld();
	static bool backlightOn = true;
	if(hidKeysDown() && kHeld)
	{
		// Adjust LCD brightness up.
		const s16 steps = g_oafConfig.backlightSteps;
		if(kHeld == (KEY_X | KEY_DUP))
			changeBacklight(steps);

		// Adjust LCD brightness down.
		if(kHeld == (KEY_X | KEY_DDOWN))
			changeBacklight(-steps);

		// Disable backlight switching in debug builds on 2DS.
		const GfxBl lcd = (MCU_getSystemModel() != SYS_MODEL_2DS ? GFX_BL_TOP : GFX_BL_BOT);
#ifndef NDEBUG
		if(lcd != GFX_BL_BOT)
#endif
		{
			// Turn off backlight.
			if(backlightOn && kHeld == (KEY_X | KEY_DLEFT))
			{
				backlightOn = false;
				GFX_powerOffBacklight(lcd);
			}

			// Turn on backlight.
			if(!backlightOn && kHeld == (KEY_X | KEY_DRIGHT))
			{
				backlightOn = true;
				GFX_powerOnBacklight(lcd);
			}
		}
	}
}

// showFileBrowser was the legacy console-based file picker. The imgui boot
// UI takes its place via oafBootUiRunMenu (which delegates to the imgui
// file browser screen and persists lastdir.txt itself).

static void rom2GameCfgPath(char romPath[512])
{
	if (g_oafConfig.useSavesFolder)
	{
		// Extract the file name and change the extension.
		// For cfg2SavePath() we need to reserve 2 extra bytes/chars.
		char tmpIniFileName[256];
		safeStrcpy(tmpIniFileName, strrchr(romPath, '/') + 1, 256 - 2);
		strcpy(tmpIniFileName + strlen(tmpIniFileName) - 4, ".ini");

		// Construct the new path.
		strcpy(romPath, OAF_SAVE_DIR "/");
		strcat(romPath, tmpIniFileName);
	}
	else
	{
		// Change the extension to .ini.
		strcpy(romPath + strlen(romPath) - 4, ".ini");
	}
}

static void gameCfg2SavePath(char cfgPath[512], const u8 saveSlot)
{
	if(saveSlot > 9)
	{
		*cfgPath = '\0'; // Prevent using the ROM as save file.
		return;
	}

	static char numberedExt[7] = {'.', 'X', '.', 's', 'a', 'v', '\0'};

	// Change the extension.
	// This relies on rom2GameCfgPath() to reserve 2 extra bytes/chars.
	numberedExt[1] = '0' + saveSlot;
	strcpy(cfgPath + strlen(cfgPath) - 4, (saveSlot == 0 ? ".sav" : numberedExt));
}

Result oafParseConfigEarly(void)
{
	Result res;
	do
	{
		// Create the work dir and switch to it.
		res = fsMakePath(OAF_WORK_DIR);
		if(res != RES_OK && res != RES_FR_EXIST) break;

		res = fChdir(OAF_WORK_DIR);
		if(res != RES_OK) break;

		// Create the saves folder.
		res = fMkdir(OAF_SAVE_DIR);
		if(res != RES_OK && res != RES_FR_EXIST) break;

		// Create screenshots folder.
		res = fMkdir(OAF_SCREENSHOT_DIR);
		if(res != RES_OK && res != RES_FR_EXIST) break;

		// Parse the config.
		res = parseOafConfig("config.ini", &g_oafConfig, true);
	} while(0);

	return res;
}

Result oafInitAndRun(bool *romLaunched)
{
	if(romLaunched != NULL) *romLaunched = false;

	Result res;
	char *const filePath = (char*)calloc(512, 1);
	if(filePath != NULL)
	{
		do
		{
			// Try to load the ROM path from autoboot.txt.
			// If this file doesn't exist show the imgui main menu.
			res = fsLoadPathFromFile("autoboot.txt", filePath);
			if(res == RES_FR_NO_FILE)
			{
				const OafBootUiResult uiRes = oafBootUiRunMenu(filePath);
				if(uiRes == OAF_UI_RESULT_EXIT)
				{
					// User chose to leave the menu — clean shutdown, not
					// an error. Caller skips the emulator loop.
					res = RES_OK;
					break;
				}
				if(uiRes != OAF_UI_RESULT_PICKED_ROM || *filePath == '\0')
				{
					res = RES_INVALID_ARG;
					break;
				}
				res = RES_OK;
			}
			else if(res != RES_OK) break;

			//make copy of rom path
			char *const romFilePath = (char*)calloc(strlen(filePath)+1, 1);
			if(romFilePath == NULL) { res = RES_OUT_OF_MEM; break; }
			strcpy(romFilePath, filePath);

			// Note: the boot UI stays alive through ROM load + save type
			// detection + patching. main.c reserved the LGY ROM zone
			// (FCRAM_BASE..+32MB) as the very first FCRAM allocation, so the
			// UI's allocations live above that zone and aren't clobbered
			// when fRead writes the ROM directly to LGY_ROM_LOC. Shutdown
			// happens just before LGY11_switchMode, below.

			// Load the ROM file.
			oafBootUiBeginProgress("Loading");
			oafBootUiUpdateProgress("Reading ROM from SD…");
			u32 romSize;
			res = loadGbaRom(filePath, &romSize);
			if(res != RES_OK) { oafBootUiEndProgress(); break; }

			// Load the per-game config.
			rom2GameCfgPath(filePath);
			res = parseOafConfig(filePath, &g_oafConfig, false);
			if(res != RES_OK && res != RES_FR_NO_FILE) break;

			// Adjust the path for the save file and get save type.
			gameCfg2SavePath(filePath, g_oafConfig.saveSlot);
			oafBootUiUpdateProgress("Detecting save type…");
			u16 saveType;
			if(g_oafConfig.saveType != 0xFF)
				saveType = g_oafConfig.saveType;
			else if(g_oafConfig.useGbaDb || g_oafConfig.saveOverride)
				saveType = getSaveType(&g_oafConfig, romSize, filePath);
			else
				saveType = detectSaveType(romSize, g_oafConfig.defaultSave);

			oafBootUiUpdateProgress("Applying patches…");
			patchRom(romFilePath, &romSize);
			free(romFilePath);
			oafBootUiEndProgress();

			// Set audio output and volume.
			CODEC_setAudioOutput(g_oafConfig.audioOut);
			CODEC_setVolumeOverride(g_oafConfig.volume);

			// Tear down the boot UI now — BEFORE OAF_videoInit. Order
			// matters: OAF_videoInit configures GBA-specific GPU state
			// (frame capture, patched gpu cmd list, gbaGfxHandler task),
			// and C3D_Fini inside oafBootUiShutdown resets GPU registers
			// in a way that clobbers that setup. The LGY ROM zone
			// reservation made in main() is intentionally NOT freed — LGY
			// hardware owns it now.
			oafBootUiShutdown();

			// Prepare ARM9 for GBA mode + save loading.
			res = LGY_prepareGbaMode(g_oafConfig.directBoot, saveType, filePath);
			if(res == RES_OK)
			{
				// Initialize video output (frame capture, post processing ect.).
				g_frameReadyEvent = OAF_videoInit();

				// Setup button overrides.
				const u32 *const maps = g_oafConfig.buttonMaps;
				u16 overrides = 0;
				for(unsigned i = 0; i < 10; i++)
					if(maps[i] != 0) overrides |= 1u<<i;
				LGY11_selectInput(overrides);

				// Sync LgyCap start with LCD VBlank.
				GFX_waitForVBlank0();
				LGY11_switchMode();

				if(romLaunched != NULL) *romLaunched = true;
			}
		} while(0);
	}
	else res = RES_OUT_OF_MEM;

	free(filePath);

	return res;
}

void oafUpdate(void)
{
	const u32 *const maps = g_oafConfig.buttonMaps;
	const u32 kHeld = hidKeysHeld();
	u16 pressed = 0;
	for(unsigned i = 0; i < 10; i++)
	{
		if((kHeld & maps[i]) != 0)
			pressed |= 1u<<i;
	}
	LGY11_setInputState(pressed);

	CODEC_runHeadphoneDetection();
	updateBacklight();
	waitForEvent(g_frameReadyEvent);
	clearEvent(g_frameReadyEvent);
}

void oafFinish(void)
{
	// frameReadyEvent deleted by this function.
	OAF_videoExit();
	g_frameReadyEvent = 0;
	LGY11_deinit();
}
