/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2022 derrek, profi200
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
#ifdef __ARM11__
#include "arm11/ui_main.h"
#endif // #ifdef __ARM11__



const char* oafResult2String(Result res)
{
	static const char *const oafResultStrings[] =
	{
		"ROM too big. Max 32 MiB",
		"Invalid patch file"
	};

	return (res < CUSTOM_ERR_OFFSET ? result2String(res) : oafResultStrings[res - CUSTOM_ERR_OFFSET]);
}

#ifdef __ARM11__
// printErrorWaitInput is the single-entry error display. The boot UI is
// always alive on ARM11 (oafBootUiInit panics on failure), so we route
// straight to the imgui modal — no libn3ds-console fallback because the
// console is never initialized.
void printErrorWaitInput(Result res, u32 waitKeys)
{
	(void)waitKeys; // imgui modal always dismisses on A.
	oafBootUiShowError(res);
}
#endif // ifdef __ARM11__