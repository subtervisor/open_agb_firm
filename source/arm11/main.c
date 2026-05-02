/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2021 derrek, profi200
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

#include "oaf_error_codes.h"
#include "fs.h"
#include "arm11/open_agb_firm.h"
#include "drivers/gfx.h"
#include "arm11/drivers/mcu.h"
#include "arm11/drivers/codec.h"
#include "arm11/drivers/hid.h"
#include "arm11/power.h"
#include "arm11/ui_main.h"
#include "arm11/allocator/fcram.h"
#include "drivers/lgy_common.h"



int main(void)
{
	// Reserve the LGY ROM zone (FCRAM_BASE..FCRAM_BASE+32MB) as the very
	// first FCRAM allocation. libn3ds' fcram pool is first-fit-from-low,
	// so this lands at FCRAM_BASE and pushes every subsequent allocation
	// (boot UI, config parser, etc.) above 0x22000000. loadGbaRom writes
	// directly to LGY_ROM_LOC = FCRAM_BASE — the allocator never tracks
	// the ROM as an alloc, but the placeholder keeps the zone marked busy
	// so nothing else lands on top of the ROM. We never read or free this
	// pointer; LGY mode owns the memory after LGY11_switchMode.
	(void)fcramMemAlign(LGY_MAX_ROM_SIZE, 0x80);

	Result res = oafParseConfigEarly();
	// Both LCDs at BGR8 so the imgui boot UI's 24-bit transfer format
	// matches the LCD readout. The GBA emulator blanks the bottom screen
	// via GFX_setForceBlack + GFX_powerOffBacklight after LGY11_switchMode,
	// so the bottom format doesn't matter for emulation.
	GFX_init(GFX_BGR8, GFX_BGR8, GFX_TOP_2D);
	changeBacklight(0); // Apply backlight config.
	//CODEC_init();

	// Imgui boot UI owns both screens until LGY11_switchMode hands the GPU
	// to the ARM9 GBA emulator. If init fails we still try to run — the
	// fallbacks in oaf_error_codes.c will catch the error path.
	(void)oafBootUiInit();

	bool romLaunched = false;
	if(res == RES_OK) res = oafInitAndRun(&romLaunched);

	if(res == RES_OK && romLaunched)
	{
		while(1)
		{
			hidScanInput();
			if(hidGetExtraKeys(0) & (KEY_POWER_HELD | KEY_POWER)) break;

			oafUpdate();
		}

		oafFinish();
	}
	else if(res != RES_OK)
	{
		// Genuine error path — show the modal. The user-chose-Exit case
		// hits the else branch with res == RES_OK and falls through to a
		// silent shutdown below.
		printErrorWaitInput(res, 0);
	}

	oafBootUiShutdown();
	CODEC_deinit();
	GFX_deinit();
	fUnmount(FS_DRIVE_SDMC); // TODO: Move elsewhere. __systemDeinit() already calls it.

	power_off();

	return 0;
}