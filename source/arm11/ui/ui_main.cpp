/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2024 derrek, profi200
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 */

#include "arm11/ui_main.h"
#include "arm11/imgui_impl_citro3d.h"
#include "arm11/imgui_impl_ctr.h"
#include "arm11/config.h"
#include "arm11/filebrowser_dirlist.h"
#include "arm11/ui_screenshot.h"
#include "imgui.h"

#include <types.h>
#include <cstdarg>
#include <cstring>

extern "C"
{
#include <citrine3d.h>
#include <drivers/gfx.h>
#include <arm11/drivers/hid.h>
#include <arm11/drivers/mcu.h>
#include <arm11/fmt.h>
#include <arm11/allocator/fcram.h>
#include <fs.h>
#include <fsutil.h>
#include <util.h>
#include <debug.h>
}


namespace
{

// Logical canvas spans the stacked top + bottom screens; top occupies
// (0, 0)–(400, 240), bottom occupies (40, 240)–(360, 480). Same layout the
// configurator uses so the rendering backend's projection matrices line up
// without changes.
constexpr float SCREEN_WIDTH  = 400.0f;
constexpr float SCREEN_HEIGHT = 480.0f;

// Bottom-screen window rect.
constexpr float BOT_X = 40.0f;
constexpr float BOT_Y = 240.0f;
constexpr float BOT_W = 320.0f;
constexpr float BOT_H = 240.0f;

constexpr u32 CLEAR_COLOR = 0x1a2529ffu;

constexpr u32 DISPLAY_TRANSFER_FLAGS =
    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
    GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

constexpr ImGuiWindowFlags FULL_SCREEN_FLAGS =
    ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoMove        |
    ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoCollapse    |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;

constexpr ImGuiWindowFlags BOTTOM_WIN_FLAGS =
    ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoResize  |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;


bool s_active = false;
C3D_RenderTarget* s_top = nullptr;
C3D_RenderTarget* s_bot = nullptr;

// Status line shown on the top "branding" bar — reflects the most recent
// outcome (loaded config / saved config / etc.). Plain ASCII, ee_snprintf'd.
char s_topStatus[96] = {0};

// Progress modal state. Active between Begin/End; Update mutates the body.
bool s_progressActive = false;
char s_progressTitle[64]   = {0};
char s_progressStatus[192] = {0};


void* imguiAlloc(size_t size, void*)
{
	return fcramAlloc(size);
}

void imguiFree(void* ptr, void*)
{
	if(ptr) fcramFree(ptr);
}

bool isNew3ds()
{
	const u8 model = (u8)MCU_getSystemModel();
	return model == 2 || model == 4;
}


// Forward decl — defined further down with the other status helpers; used
// by endFrame() to surface screenshot success/error feedback.
void setTopStatus(const char *fmt, ...);


void beginFrame()
{
	hidScanInput();

	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C3D_RenderTargetClear(s_top, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
	C3D_RenderTargetClear(s_bot, C3D_CLEAR_ALL, CLEAR_COLOR, 0);

	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(SCREEN_WIDTH, SCREEN_HEIGHT);
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

	ImGui_ImplCitro3D_NewFrame();
	ImGui_ImplCtr_NewFrame();
	ImGui::NewFrame();
}

void endFrame()
{
	ImGui::Render();
	ImGui_ImplCitro3D_RenderDrawData(ImGui::GetDrawData(), s_top, s_bot);

	// HOME = dump both screens to BMP. Latch the framebuffer pointers
	// before C3D_FrameEnd's swap (the captured ones are the buffers we just
	// rendered into; after the swap they become the front buffers, but the
	// memory itself is still valid for read).
	const bool wantShot = (hidGetExtraKeys(KEY_HOME) & KEY_HOME) != 0;
	const void *topFb = NULL;
	const void *botFb = NULL;
	if(wantShot)
	{
		topFb = GFX_getBuffer(GFX_LCD_TOP, GFX_SIDE_LEFT);
		botFb = GFX_getBuffer(GFX_LCD_BOT, GFX_SIDE_LEFT);
	}

	C3D_FrameEnd(0);

	if(wantShot && topFb != NULL && botFb != NULL)
	{
		char path[96];
		const Result r = oafBootUiScreenshot(topFb, botFb, path, sizeof(path));
		if(r == RES_OK) setTopStatus("Screenshot saved: %s", path);
		else            setTopStatus("Screenshot error 0x%08lX", r);
	}
}

bool powerRequested()
{
	return (hidGetExtraKeys(0) & (KEY_POWER_HELD | KEY_POWER)) != 0;
}


void drawTopBrand()
{
	ImGui::SetNextWindowPos (ImVec2(0.0f, 0.0f), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(SCREEN_WIDTH, SCREEN_HEIGHT * 0.5f), ImGuiCond_Always);
	ImGui::Begin("##top", nullptr, FULL_SCREEN_FLAGS);
	ImGui::TextUnformatted("open_agb_firm");
	if(s_topStatus[0] != '\0')
	{
		ImGui::Spacing();
		ImGui::TextWrapped("%s", s_topStatus);
	}
	ImGui::End();
}

void drawProgressIfActive()
{
	if(!s_progressActive) return;
	ImGui::SetNextWindowPos (ImVec2(BOT_X, BOT_Y), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(BOT_W, BOT_H), ImGuiCond_Always);
	ImGui::Begin(s_progressTitle, nullptr, BOTTOM_WIN_FLAGS);
	ImGui::TextWrapped("%s", s_progressStatus);
	ImGui::End();
}

void setTopStatus(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	ee_vsnprintf(s_topStatus, sizeof(s_topStatus), fmt, ap);
	va_end(ap);
}


// --- Settings screen ------------------------------------------------------
//
// Mutates g_oafConfig in place; on Save calls writeOafConfig and updates the
// top-bar status to reflect success/failure.

constexpr const char *SCALER_LABELS[] = {
	"None", "Bilinear (1.5x)", "Matrix (1.5x)"
};
constexpr const char *COLOR_PROFILE_LABELS[] = {
	"None", "GBA", "GB Micro", "GBA SP (AGS-101)",
	"Nintendo DS (phat)", "Nintendo DS Lite",
	"Nintendo Switch Online", "Visual Boy Advance / No$GBA",
	"Identity",
};
constexpr const char *AUDIO_OUT_LABELS[] = {
	"Automatic", "3DS Speakers", "Headphones",
};
constexpr const char *SAVE_TYPE_LABELS[] = {
	"EEPROM 8K", "EEPROM 8K (256M ROM)",
	"EEPROM 64K", "EEPROM 64K (256M ROM)",
	"Flash 512K Atmel + RTC", "Flash 512K Atmel",
	"Flash 512K SST + RTC", "Flash 512K SST",
	"Flash 512K Panasonic + RTC", "Flash 512K Panasonic",
	"Flash 1M Macronix + RTC", "Flash 1M Macronix",
	"Flash 1M Sanyo + RTC", "Flash 1M Sanyo",
	"SRAM 256K", "None",
};

template<typename ToInt>
bool comboU8(const char *label, ToInt *value, const char *const *names, int count)
{
	int idx = (int)*value;
	if(idx < 0 || idx >= count) idx = 0;
	if(ImGui::Combo(label, &idx, names, count))
	{
		*value = (ToInt)idx;
		return true;
	}
	return false;
}

// Returns true if the user pressed Back (B) or selected the Back button.
bool runSettingsScreen()
{
	const int blMin = isNew3ds() ? 16 : 20;
	const int blMax = isNew3ds() ? 142 : 117;

	// Snapshot the backlight on entry. The slider applies changes live to
	// the LCD (GFX_setLcdLuminance), so backing out without saving needs to
	// restore this value. Updated on Save so further edits revert to the
	// just-saved value.
	u8 baselineBacklight = g_oafConfig.backlight;

	while(true)
	{
		if(powerRequested()) return true;

		beginFrame();
		drawTopBrand();

		ImGui::SetNextWindowPos (ImVec2(BOT_X, BOT_Y), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(BOT_W, BOT_H), ImGuiCond_Always);
		ImGui::Begin("Settings", nullptr, BOTTOM_WIN_FLAGS);

		// Stylus drag-to-scroll: the CTR backend's touch state machine
		// accumulates a vertical drag delta in pixels; apply it to this
		// window's scroll position so dragging the settings list scrolls
		// it the same way the file browser does.
		const float dragDy = ImGui_ImplCtr_ConsumePendingScrollY();
		if(dragDy != 0.0f)
			ImGui::SetScrollY(ImGui::GetScrollY() - dragDy);

		ImGui::TextUnformatted("General");
		ImGui::Separator();
		{
			int v = g_oafConfig.backlight;
			if(ImGui::SliderInt("Backlight", &v, blMin, blMax, "%d", ImGuiSliderFlags_AlwaysClamp))
			{
				g_oafConfig.backlight = (u8)v;
				// Live preview — apply the new luminance to the LCD as soon
				// as the slider moves so the user sees the brightness
				// change without waiting for Save.
				GFX_setLcdLuminance((u32)v);
			}
		}
		{
			int v = g_oafConfig.backlightSteps;
			if(ImGui::SliderInt("Backlight Steps", &v, 1, 128, "%d", ImGuiSliderFlags_AlwaysClamp))
				g_oafConfig.backlightSteps = (u8)v;
		}
		ImGui::Checkbox("Direct Boot",      &g_oafConfig.directBoot);
		ImGui::Checkbox("Use GBA DB",       &g_oafConfig.useGbaDb);
		ImGui::Checkbox("Use Saves Folder", &g_oafConfig.useSavesFolder);

		ImGui::Spacing();
		ImGui::TextUnformatted("Video");
		ImGui::Separator();
		comboU8("Scaler",        &g_oafConfig.scaler,       SCALER_LABELS,        IM_ARRAYSIZE(SCALER_LABELS));
		comboU8("Color Profile", &g_oafConfig.colorProfile, COLOR_PROFILE_LABELS, IM_ARRAYSIZE(COLOR_PROFILE_LABELS));
		ImGui::SliderFloat("Contrast",   &g_oafConfig.contrast,   0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat("Brightness", &g_oafConfig.brightness, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat("Saturation", &g_oafConfig.saturation, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);

		ImGui::Spacing();
		ImGui::TextUnformatted("Audio");
		ImGui::Separator();
		comboU8("Audio Output", &g_oafConfig.audioOut, AUDIO_OUT_LABELS, IM_ARRAYSIZE(AUDIO_OUT_LABELS));
		bool sliderVolume = (g_oafConfig.volume > -20);
		if(ImGui::Checkbox("Use system volume slider", &sliderVolume))
			g_oafConfig.volume = sliderVolume ? 127 : -20;
		if(!sliderVolume)
		{
			int v = g_oafConfig.volume;
			if(ImGui::SliderInt("Volume (dB)", &v, -128, -20, "%d", ImGuiSliderFlags_AlwaysClamp))
				g_oafConfig.volume = (s8)v;
		}

		ImGui::Spacing();
		ImGui::TextUnformatted("Advanced");
		ImGui::Separator();
		ImGui::Checkbox("Save type override on ROM load", &g_oafConfig.saveOverride);
		comboU8("Default Save Type", &g_oafConfig.defaultSave, SAVE_TYPE_LABELS, IM_ARRAYSIZE(SAVE_TYPE_LABELS));

		ImGui::Spacing();
		ImGui::Separator();
		bool save = ImGui::Button("Save");
		ImGui::SameLine();
		bool back = ImGui::Button("Back");

		ImGui::End();
		endFrame();

		if(save)
		{
			const Result r = writeOafConfig(OAF_WORK_DIR "/config.ini", &g_oafConfig);
			setTopStatus(r == RES_OK ? "Configuration saved." : "Failed to save configuration.");
			if(r == RES_OK) baselineBacklight = g_oafConfig.backlight;
		}
		if(back || (hidKeysDown() & KEY_B))
		{
			// Revert the live backlight if the user backed out without
			// saving — otherwise the LCD stays at whatever the slider was
			// last dragged to even though config.ini still has the old
			// value.
			if(g_oafConfig.backlight != baselineBacklight)
			{
				g_oafConfig.backlight = baselineBacklight;
				GFX_setLcdLuminance((u32)baselineBacklight);
			}
			return true;
		}
	}
}


// --- File browser ---------------------------------------------------------

constexpr const char *LASTDIR_FILE = "lastdir.txt";

void cdInto(char *curDir, size_t cap, const char *child)
{
	const size_t pathLen = strlen(curDir);
	size_t writeAt = pathLen;
	if(pathLen == 0 || curDir[pathLen - 1] != '/')
	{
		if(writeAt + 1 < cap) curDir[writeAt++] = '/';
		curDir[writeAt] = '\0';
	}
	const size_t childLen = strlen(child);
	if(writeAt + childLen + 1 > cap) return; // truncate quietly
	std::memcpy(curDir + writeAt, child, childLen);
	curDir[writeAt + childLen] = '\0';
}

void cdParent(char *curDir)
{
	const size_t pathLen = strlen(curDir);
	if(pathLen == 0) return;
	char *p = curDir + pathLen;
	while(p > curDir && *--p != '/') {}
	if(p > curDir && p[-1] == ':')
	{
		// Don't strip the slash after "sdmc:" — leave as "sdmc:/".
		p[1] = '\0';
	}
	else
	{
		*p = '\0';
	}
}

// File browser is the boot UI's entry screen. A = activate (enter dir or
// pick file), B = parent dir, Y = open the in-window menu (Settings/Exit).
//
// Returns OAF_UI_RESULT_PICKED_ROM when the user picks a .gba file,
// OAF_UI_RESULT_EXIT when they pick Exit from the menu (or hold Power),
// OAF_UI_RESULT_ERROR on init failure.
OafBootUiResult runFileBrowser(char outRomPath[512])
{
	char *curDir = (char*)fcramAlloc(512);
	FbDirList *dList = (FbDirList*)fcramAlloc(sizeof(FbDirList));
	if(curDir == NULL || dList == NULL)
	{
		if(curDir) fcramFree(curDir);
		if(dList) fcramFree(dList);
		setTopStatus("Out of memory opening file browser.");
		return OAF_UI_RESULT_ERROR;
	}

	if(fsLoadPathFromFile(LASTDIR_FILE, curDir) != RES_OK)
		safeStrcpy(curDir, "sdmc:/", 512);

	Result scanRes = fbScanDir(curDir, dList, ".gba");
	if(scanRes != RES_OK)
	{
		// Last dir went stale — fall back to root.
		safeStrcpy(curDir, "sdmc:/", 512);
		scanRes = fbScanDir(curDir, dList, ".gba");
	}

	int  selected   = 0;        // index into dList->ptrs
	bool needRescan = false;
	bool openMenu   = false;    // OpenPopup queued for next frame
	OafBootUiResult result = OAF_UI_RESULT_EXIT;

	while(true)
	{
		if(powerRequested())
		{
			result = OAF_UI_RESULT_EXIT;
			break;
		}

		if(needRescan)
		{
			scanRes = fbScanDir(curDir, dList, ".gba");
			selected = 0;
			needRescan = false;
		}

		beginFrame();

		// Top: current path + brand.
		ImGui::SetNextWindowPos (ImVec2(0.0f, 0.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(SCREEN_WIDTH, SCREEN_HEIGHT * 0.5f), ImGuiCond_Always);
		ImGui::Begin("##browser_top", nullptr, FULL_SCREEN_FLAGS);
		ImGui::TextUnformatted("Select a ROM   (A: open  B: up  Y: menu)");
		ImGui::Spacing();
		ImGui::TextWrapped("%s", curDir);
		if(scanRes != RES_OK)
		{
			ImGui::Spacing();
			ImGui::Text("Scan error: 0x%08lX", (unsigned long)scanRes);
		}
		ImGui::End();

		// Bottom: file list with menu bar at top. The menu bar gives us a
		// touch target ("Menu" label) and Y opens it via gamepad.
		ImGui::SetNextWindowPos (ImVec2(BOT_X, BOT_Y), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(BOT_W, BOT_H), ImGuiCond_Always);
		ImGui::Begin("Files", nullptr,
		             BOTTOM_WIN_FLAGS | ImGuiWindowFlags_NoScrollbar |
		             ImGuiWindowFlags_MenuBar);

		bool wantSettings = false;
		bool wantExit     = false;
		if(ImGui::BeginMenuBar())
		{
			// "\xee\x80\x83" = U+E003 = Y-button glyph from the 3DS sysfont
			// PUA range. Mirrors the configurator's menu label.
			if(ImGui::BeginMenu("Menu \xee\x80\x83"))
			{
				if(ImGui::MenuItem("Settings")) wantSettings = true;
				if(ImGui::MenuItem("Exit"))     wantExit     = true;
				ImGui::EndMenu();
			}
			ImGui::EndMenuBar();
		}

		// Selectable list. NavFlattened so dpad up/down works on the list
		// without needing X to "enter scope".
		ImGui::BeginChild("##list", ImVec2(0.0f, 0.0f),
		                  ImGuiChildFlags_NavFlattened);
		// Apply any pending stylus-drag scroll. The CTR backend's touch
		// state machine accumulates a vertical delta when the user drags
		// the stylus inside the bottom screen; we feed that into this
		// child's scroll so dragging the file list scrolls it.
		const float dragDy = ImGui_ImplCtr_ConsumePendingScrollY();
		if(dragDy != 0.0f)
			ImGui::SetScrollY(ImGui::GetScrollY() - dragDy);
		const u32 num = dList->num;
		bool itemActivated = false;
		for(u32 i = 0; i < num; ++i)
		{
			const char *raw = dList->ptrs[i];
			const bool isDir = (*raw == FB_ENT_TYPE_DIR);
			const char *name = raw + 1;
			char label[300];
			ee_snprintf(label, sizeof(label), isDir ? "[D] %s" : "    %s", name);
			const bool wasSelected = (selected == (int)i);
			if(ImGui::Selectable(label, wasSelected))
			{
				selected = (int)i;
				itemActivated = true;
			}
			if(wasSelected && i == 0)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndChild();

		ImGui::End();

		// Y opens the menu bar's first menu by triggering its popup. Latch
		// to a flag and OpenPopup next frame so the menu shows and gamepad
		// nav lands in it.
		if(hidKeysDown() & KEY_Y) openMenu = true;
		if(openMenu)
		{
			ImGui::OpenPopup("Menu");
			openMenu = false;
		}

		endFrame();

		// Y-menu actions handled outside the menu rendering so we don't
		// mutate state mid-frame.
		if(wantExit)
		{
			result = OAF_UI_RESULT_EXIT;
			break;
		}
		if(wantSettings)
		{
			(void)runSettingsScreen();
			continue;
		}

		const bool act = itemActivated;
		const bool par = (hidKeysDown() & KEY_B) != 0;

		if(par)
		{
			cdParent(curDir);
			needRescan = true;
			continue;
		}
		if(act && num != 0 && selected >= 0 && (u32)selected < num)
		{
			const char *raw = dList->ptrs[selected];
			const bool isDir = (*raw == FB_ENT_TYPE_DIR);
			const char *name = raw + 1;
			cdInto(curDir, 512, name);
			if(isDir)
			{
				needRescan = true;
				continue;
			}
			// File picked.
			safeStrcpy(outRomPath, curDir, 512);

			// Persist the parent dir for next launch.
			char *slash = strrchr(curDir, '/');
			if(slash != NULL)
			{
				size_t parentLen = (size_t)(slash - curDir);
				if(parentLen > 0 && curDir[parentLen - 1] == ':') parentLen++; // keep "sdmc:/"
				if(parentLen < 512)
				{
					curDir[parentLen] = '\0';
					(void)fsQuickWrite(LASTDIR_FILE, curDir, parentLen + 1);
				}
			}
			result = OAF_UI_RESULT_PICKED_ROM;
			break;
		}
	}

	fcramFree(dList);
	fcramFree(curDir);
	return result;
}

} // namespace


extern "C"
{

Result oafBootUiInit(void)
{
	if(s_active) return RES_OK;

	C3D_Init(2 * C3D_DEFAULT_CMDBUF_SIZE);

	s_top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
	if(s_top == NULL) panicMsg("Boot UI: failed to allocate top render target");
	C3D_RenderTargetSetOutput(s_top, GFX_LCD_TOP, GFX_SIDE_LEFT, DISPLAY_TRANSFER_FLAGS);

	s_bot = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH16);
	if(s_bot == NULL) panicMsg("Boot UI: failed to allocate bottom render target");
	C3D_RenderTargetSetOutput(s_bot, GFX_LCD_BOT, GFX_SIDE_LEFT, DISPLAY_TRANSFER_FLAGS);

	ImGui::SetAllocatorFunctions(imguiAlloc, imguiFree);
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGuiIO& io = ImGui::GetIO();
	ImGuiStyle& style = ImGui::GetStyle();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(SCREEN_WIDTH, SCREEN_HEIGHT);
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	style.ScaleAllSizes(0.5f);
	style.WindowRounding = 0.0f;
	style.FrameRounding  = 1.0f;
	style.AntiAliasedLines       = false;
	style.AntiAliasedLinesUseTex = false;
	style.AntiAliasedFill        = false;

	ImGui_ImplCtr_Init();
	ImGui_ImplCitro3D_Init();

	s_progressActive = false;
	s_progressTitle[0]  = '\0';
	s_progressStatus[0] = '\0';
	s_topStatus[0]      = '\0';

	s_active = true;
	return RES_OK;
}

void oafBootUiShutdown(void)
{
	if(!s_active) return;

	ImGui_ImplCitro3D_Shutdown();
	ImGui_ImplCtr_Shutdown();
	ImGui::DestroyContext();

	if(s_bot != NULL) { C3D_RenderTargetDelete(s_bot); s_bot = NULL; }
	if(s_top != NULL) { C3D_RenderTargetDelete(s_top); s_top = NULL; }

	C3D_Fini();
	s_active = false;
}

OafBootUiResult oafBootUiRunMenu(char outRomPath[512])
{
	if(!s_active)
	{
		if(outRomPath != NULL) outRomPath[0] = '\0';
		return OAF_UI_RESULT_EXIT;
	}
	if(outRomPath != NULL) outRomPath[0] = '\0';
	// File browser is the entry screen; settings/exit are reachable via the
	// in-window Y menu.
	return runFileBrowser(outRomPath);
}

void oafBootUiBeginProgress(const char *title)
{
	if(!s_active || title == NULL) return;
	std::strncpy(s_progressTitle, title, sizeof(s_progressTitle) - 1);
	s_progressTitle[sizeof(s_progressTitle) - 1] = '\0';
	s_progressStatus[0] = '\0';
	s_progressActive = true;

	beginFrame();
	drawTopBrand();
	drawProgressIfActive();
	endFrame();
}

void oafBootUiUpdateProgress(const char *statusFmt, ...)
{
	if(!s_active || statusFmt == NULL) return;

	va_list ap;
	va_start(ap, statusFmt);
	ee_vsnprintf(s_progressStatus, sizeof(s_progressStatus), statusFmt, ap);
	va_end(ap);

	beginFrame();
	drawTopBrand();
	drawProgressIfActive();
	endFrame();
}

void oafBootUiEndProgress(void)
{
	if(!s_active) return;
	s_progressActive = false;
	s_progressTitle[0]  = '\0';
	s_progressStatus[0] = '\0';
}

void oafBootUiSaveTypeOverride(u16 autoDetected, u16 dbType, bool dbFound,
                               bool saveExists,
                               u16 *outSaveType, bool *outDeleteRequested)
{
	// Defensive defaults: no override, no delete.
	if(outSaveType        != NULL) *outSaveType        = autoDetected;
	if(outDeleteRequested != NULL) *outDeleteRequested = false;

	if(!s_active) return;

	// 8 cursor positions -> raw save-type id. Mirrors the pre-imgui table
	// in save_type.c.
	static const u8 cursorToSaveType[8] = {0, 2, 8, 9, 10, 11, 14, 15};
	static const u8 saveTypeToCursor[16] =
	    {0, 0, 1, 1, 2, 3, 2, 3, 2, 3, 4, 5, 4, 5, 6, 7};
	static const char *const labels[8] = {
	    "EEPROM 8K",
	    "EEPROM 64K",
	    "Flash 512K + RTC",
	    "Flash 512K",
	    "Flash 1M + RTC",
	    "Flash 1M",
	    "SRAM 256K",
	    "None",
	};

	// Pick initial cursor: prefer gba_db value, fall back to autodetect.
	int cursor = saveTypeToCursor[(dbFound ? dbType : autoDetected) & 0xF];
	bool deleted = saveExists ? false : true; // already gone -> reflect in label

	while(true)
	{
		if(powerRequested()) break;

		beginFrame();
		drawTopBrand();

		ImGui::SetNextWindowPos (ImVec2(BOT_X, BOT_Y), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(BOT_W, BOT_H), ImGuiCond_Always);
		ImGui::Begin("Save Type Override", nullptr,
		             BOTTOM_WIN_FLAGS | ImGuiWindowFlags_NoScrollbar);

		ImGui::Text("Save file: %s", deleted ? "Deleted" : (saveExists ? "Found" : "Not found"));
		ImGui::Text("Auto-detected: %u", (unsigned)autoDetected);
		if(dbFound) ImGui::Text("From gba_db.bin: %u", (unsigned)dbType);
		else        ImGui::TextUnformatted("From gba_db.bin: not found");
		ImGui::Separator();

		const float footerH = ImGui::GetFrameHeightWithSpacing();
		ImGui::BeginChild("##stlist", ImVec2(0.0f, -footerH),
		                  ImGuiChildFlags_NavFlattened);
		for(int i = 0; i < 8; ++i)
		{
			if(ImGui::Selectable(labels[i], cursor == i))
				cursor = i;
			if(cursor == i && i == 0)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndChild();

		ImGui::Separator();
		const bool ok    = ImGui::Button("OK");
		ImGui::SameLine();
		bool del = false;
		if(saveExists && !deleted)
		{
			del = ImGui::Button("Delete save");
			ImGui::SameLine();
		}
		const bool cancel = ImGui::Button("Cancel");

		ImGui::End();
		endFrame();

		const u32 kDown = hidKeysDown();
		// X also triggers delete (mirrors the legacy console menu).
		if(saveExists && !deleted && (del || (kDown & KEY_X)))
			deleted = true;

		if(cancel || (kDown & KEY_B)) return; // outputs already at defaults
		if(ok || (kDown & KEY_A))
		{
			if(outSaveType != NULL)
				*outSaveType = cursorToSaveType[cursor & 7];
			if(outDeleteRequested != NULL)
				*outDeleteRequested = deleted && saveExists;
			return;
		}
	}
}

} // extern "C"

namespace {

// Drive a modal frame loop until the user dismisses it with A (or holds
// Power). Used by both the error and message paths.
void runModalUntilDismissed(const char *title, const char *body)
{
	(void)hidGetExtraKeys(KEY_POWER);
	if(!s_active) return;

	while(true)
	{
		beginFrame();
		drawTopBrand();

		ImGui::SetNextWindowPos (ImVec2(BOT_X, BOT_Y), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(BOT_W, BOT_H), ImGuiCond_Always);
		ImGui::Begin(title, nullptr, BOTTOM_WIN_FLAGS);
		ImGui::TextWrapped("%s", body);
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextUnformatted("A: continue   POWER: exit");
		ImGui::End();

		endFrame();

		if(hidKeysDown() & KEY_A) break;
		if(powerRequested()) break;
	}
}

} // namespace

extern "C" {

void oafBootUiShowError(Result res)
{
	if(!s_active) return;
	char msg[128];
	ee_snprintf(msg, sizeof(msg), "%s", oafResult2String(res));
	runModalUntilDismissed("Error", msg);
}

void oafBootUiShowMessage(const char *fmt, ...)
{
	if(!s_active || fmt == NULL) return;

	char msg[256];
	va_list ap;
	va_start(ap, fmt);
	ee_vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	runModalUntilDismissed("Notice", msg);
}

} // extern "C"
