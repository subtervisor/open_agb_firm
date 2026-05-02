/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2024 profi200
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
#include "arm11/config.h"
#include "tini/tini.h"
#include "util.h"
#include "fsutil.h"
#include "arm11/fmt.h"
#include "arm11/allocator/fcram.h"


// Initial INI buffer used for read; grown by tini as needed via the allocator.
#define INI_BUF_SIZE  (1024u)
// Headroom for serialization; the full config tops out around 600 bytes.
#define INI_OUT_SIZE  (2048u)


// Save-type slot strings, indexed by the u8 saveType / defaultSave value.
static const char *const kSaveTypeNames[16] =
{
	"eeprom_8k",
	"rom_256m_eeprom_8k",
	"eeprom_64k",
	"rom_256m_eeprom_64k",
	"flash_512k_atmel_rtc",
	"flash_512k_atmel",
	"flash_512k_sst_rtc",
	"flash_512k_sst",
	"flash_512k_panasonic_rtc",
	"flash_512k_panasonic",
	"flash_1m_macronix_rtc",
	"flash_1m_macronix",
	"flash_1m_sanyo_rtc",
	"flash_1m_sanyo",
	"sram_256k",
	"none"
};

static const char *const kScalerNames[3]      = {"none", "bilinear", "matrix"};
static const char *const kColorProfileNames[9] =
	{"none", "gba", "gb_micro", "gba_sp101", "nds", "ds_lite", "nso", "vba", "identity"};
static const char *const kAudioOutNames[3]    = {"auto", "speakers", "headphones"};


// Default config.
OafConfig g_oafConfig =
{
	// [general]
	64,    // backlight
	5,     // backlightSteps
	false, // directBoot
	true,  // useGbaDb
	true,  // useSavesFolder

	// [video]
	2,     // scaler
	0,     // colorProfile
	1.f,   // contrast
	0.f,   // brightness
	1.f,   // saturation

	// [audio]
	0,     // Automatic audio output.
	127,   // Control via volume slider.

	// [input]
	{      // buttonMaps
		0, // A
		0, // B
		0, // Select
		0, // Start
		0, // Right
		0, // Left
		0, // Up
		0, // Down
		0, // R
		0  // L
	},

	// [game]
	0,     // saveSlot
	255,   // saveType

	// [advanced]
	false, // saveOverride
	14     // defaultSave
};



// FCRAM-backed allocator for tini documents — keeps INI state out of the
// AXIWRAM heap.
static void *tiniAlloc(size_t size, void *user)
{
	(void)user;
	return fcramAlloc(size);
}

static void tiniFree(void *ptr, void *user)
{
	(void)user;
	if(ptr) fcramFree(ptr);
}

static const tini_allocator s_tiniAlloc =
{
	.alloc = tiniAlloc,
	.free  = tiniFree,
	.user  = NULL,
};


static u32 parseButtons(const char *str)
{
	if(str == NULL || *str == '\0') return 0;

	char buf[32]; // Should be enough for all useful mappings.
	buf[31] = '\0';
	strncpy(buf, str, 31);

	char *bufPtr = buf;
	static const char *const buttonStrLut[32] =
	{
		"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN",
		"R", "L", "X", "Y", "", "", "ZL", "ZR",
		"", "", "", "", "TOUCH", "", "", "",
		"CS_RIGHT", "CS_LEFT", "CS_UP", "CS_DOWN", "CP_RIGHT", "CP_LEFT", "CP_UP", "CP_DOWN"
	};
	u32 map = 0;
	while(1)
	{
		char *const nextDelimiter = strchr(bufPtr, ',');
		if(nextDelimiter != NULL) *nextDelimiter = '\0';

		unsigned i = 0;
		while(i < 32 && strcmp(buttonStrLut[i], bufPtr) != 0) ++i;
		if(i == 32) break;
		map |= 1u<<i;

		if(nextDelimiter == NULL) break;

		bufPtr = nextDelimiter + 1; // Skip delimiter.
	}

	// Empty strings will match the entry for bit 12.
	return map & ~(1u<<12);
}

// Find the index of `value` in `table`. Returns -1 if no match.
static int lookupName(const char *value, const char *const *table, int count)
{
	if(value == NULL) return -1;
	for(int i = 0; i < count; ++i)
		if(strcmp(value, table[i]) == 0) return i;
	return -1;
}

static void getStr(const tini *doc, const char *section, const char *key,
                   const char *const *table, int count, u8 *outIdx)
{
	const int i = lookupName(tini_get(doc, section, key), table, count);
	if(i >= 0) *outIdx = (u8)i;
}

static void getBool(const tini *doc, const char *section, const char *key, bool *out)
{
	const char *v = tini_get(doc, section, key);
	if(v == NULL) return;
	if(strcmp(v, "true")  == 0) *out = true;
	else if(strcmp(v, "false") == 0) *out = false;
}

static void getU8(const tini *doc, const char *section, const char *key, u8 *out)
{
	const char *v = tini_get(doc, section, key);
	if(v != NULL) *out = (u8)strtoul(v, NULL, 10);
}

static void getS8(const tini *doc, const char *section, const char *key, s8 *out)
{
	const char *v = tini_get(doc, section, key);
	if(v != NULL) *out = (s8)strtol(v, NULL, 10);
}

static void getFloat(const tini *doc, const char *section, const char *key, float *out)
{
	const char *v = tini_get(doc, section, key);
	if(v != NULL) *out = str2float(v);
}

// "%u.%03u" expansion sized for [0, 9.999]. Avoids pulling in newlib's float
// printer; the configurator uses the same trick.
static void formatFloat3(char buf[16], float value)
{
	if(value < 0.0f) value = 0.0f;
	const unsigned scaled = (unsigned)(value * 1000.0f + 0.5f);
	ee_snprintf(buf, 16, "%u.%03u", scaled / 1000u, scaled % 1000u);
}

static void formatU8(char buf[16], u32 value)
{
	ee_snprintf(buf, 16, "%lu", value);
}

static void formatS8(char buf[16], s32 value)
{
	ee_snprintf(buf, 16, "%ld", value);
}


static void loadFromTini(const tini *doc, OafConfig *cfg)
{
	// [general]
	getU8  (doc, "general", "backlight",      &cfg->backlight);
	getU8  (doc, "general", "backlightSteps", &cfg->backlightSteps);
	getBool(doc, "general", "directBoot",     &cfg->directBoot);
	getBool(doc, "general", "useGbaDb",       &cfg->useGbaDb);
	getBool(doc, "general", "useSavesFolder", &cfg->useSavesFolder);

	// [video]
	getStr  (doc, "video", "scaler",       kScalerNames,       3, &cfg->scaler);
	getStr  (doc, "video", "colorProfile", kColorProfileNames, 9, &cfg->colorProfile);
	getFloat(doc, "video", "contrast",     &cfg->contrast);
	getFloat(doc, "video", "brightness",   &cfg->brightness);
	getFloat(doc, "video", "saturation",   &cfg->saturation);

	// [audio]
	getStr(doc, "audio", "audioOut", kAudioOutNames, 3, &cfg->audioOut);
	getS8 (doc, "audio", "volume",   &cfg->volume);

	// [input] — uses parseButtons() on both the key and the value (the value
	// can be a comma-separated list of buttons that all map to the GBA button
	// named in the key). Tini exposes this through the iterator API.
	const size_t entryCount = tini_count(doc);
	for(size_t i = 0; i < entryCount; ++i)
	{
		tini_entry e = tini_at(doc, i);
		if(e.section == NULL || strcmp(e.section, "input") != 0) continue;

		const u32 button = parseButtons(e.key) & 0x3FFu; // Only allow GBA buttons.
		if(button == 0) continue;

		// Highest set bit picks the GBA target if the user wrote multiple.
		const u32 shift = 31u - __builtin_clzl(button);
		cfg->buttonMaps[shift] = parseButtons(e.value);
	}

	// [game]
	getU8(doc, "game", "saveSlot", &cfg->saveSlot);
	{
		const char *v = tini_get(doc, "game", "saveType");
		if(v != NULL)
		{
			if(strcmp(v, "auto") == 0)       cfg->saveType = 255;
			else
			{
				const int idx = lookupName(v, kSaveTypeNames, 16);
				if(idx >= 0) cfg->saveType = (u8)idx;
			}
		}
	}

	// [advanced]
	getBool(doc, "advanced", "saveOverride", &cfg->saveOverride);
	{
		const char *v = tini_get(doc, "advanced", "defaultSave");
		const int idx = lookupName(v, kSaveTypeNames, 16);
		if(idx >= 0) cfg->defaultSave = (u16)idx;
	}
}


// Replace the document with values from `cfg`. Order of tini_set calls fixes
// the section/key order in the serialized output.
static int storeToTini(tini *doc, const OafConfig *cfg)
{
	char tmp[16];
	int rc = 0;

	// [general]
	formatU8(tmp, cfg->backlight);      if((rc = tini_set(doc, "general", "backlight", tmp))      < 0) return rc;
	formatU8(tmp, cfg->backlightSteps); if((rc = tini_set(doc, "general", "backlightSteps", tmp)) < 0) return rc;
	if((rc = tini_set(doc, "general", "directBoot",     cfg->directBoot     ? "true" : "false")) < 0) return rc;
	if((rc = tini_set(doc, "general", "useGbaDb",       cfg->useGbaDb       ? "true" : "false")) < 0) return rc;
	if((rc = tini_set(doc, "general", "useSavesFolder", cfg->useSavesFolder ? "true" : "false")) < 0) return rc;

	// [video]
	if((rc = tini_set(doc, "video", "scaler",       kScalerNames[cfg->scaler < 3 ? cfg->scaler : 0])) < 0) return rc;
	if((rc = tini_set(doc, "video", "colorProfile", kColorProfileNames[cfg->colorProfile < 9 ? cfg->colorProfile : 0])) < 0) return rc;
	formatFloat3(tmp, cfg->contrast);   if((rc = tini_set(doc, "video", "contrast",   tmp)) < 0) return rc;
	formatFloat3(tmp, cfg->brightness); if((rc = tini_set(doc, "video", "brightness", tmp)) < 0) return rc;
	formatFloat3(tmp, cfg->saturation); if((rc = tini_set(doc, "video", "saturation", tmp)) < 0) return rc;

	// [audio]
	if((rc = tini_set(doc, "audio", "audioOut", kAudioOutNames[cfg->audioOut < 3 ? cfg->audioOut : 0])) < 0) return rc;
	formatS8(tmp, cfg->volume); if((rc = tini_set(doc, "audio", "volume", tmp)) < 0) return rc;

	// [advanced]
	if((rc = tini_set(doc, "advanced", "saveOverride", cfg->saveOverride ? "true" : "false")) < 0) return rc;
	if((rc = tini_set(doc, "advanced", "defaultSave",  kSaveTypeNames[cfg->defaultSave < 16 ? cfg->defaultSave : 14])) < 0) return rc;

	return 0;
}


Result parseOafConfig(const char *const path, OafConfig *cfg, const bool newCfgOnError)
{
	cfg = (cfg != NULL ? cfg : &g_oafConfig);

	char *iniBuf = (char*)fcramAlloc(INI_BUF_SIZE);
	if(iniBuf == NULL) return RES_OUT_OF_MEM;

	Result res = fsQuickRead(path, iniBuf, INI_BUF_SIZE - 1);
	if(res == RES_OK)
	{
		// Read length isn't returned; tini_parse stops at the first NUL or
		// embedded section break, and fsQuickRead leaves the buffer zeroed
		// past the read range (calloc-equivalent semantics from fcramAlloc).
		// Pass strlen so tini sees only the real bytes.
		tini *doc = tini_create(&s_tiniAlloc);
		if(doc != NULL)
		{
			(void)tini_parse(doc, iniBuf, strlen(iniBuf));
			loadFromTini(doc, cfg);
			tini_destroy(doc);
		}
	}
	else if(newCfgOnError)
	{
		// Build a fresh document from the in-memory defaults and write it.
		res = writeOafConfig(path, cfg);
	}

	fcramFree(iniBuf);
	return res;
}

Result writeOafConfig(const char *const path, const OafConfig *cfg)
{
	if(cfg == NULL) cfg = &g_oafConfig;

	tini *doc = tini_create(&s_tiniAlloc);
	if(doc == NULL) return RES_OUT_OF_MEM;

	if(storeToTini(doc, cfg) < 0)
	{
		tini_destroy(doc);
		return RES_OUT_OF_MEM;
	}

	const size_t needed = tini_serialize(doc, NULL);
	if(needed == 0 || needed >= INI_OUT_SIZE)
	{
		tini_destroy(doc);
		return RES_OUT_OF_RANGE;
	}

	char *out = (char*)fcramAlloc(needed + 1);
	if(out == NULL)
	{
		tini_destroy(doc);
		return RES_OUT_OF_MEM;
	}

	tini_serialize(doc, out);
	tini_destroy(doc);

	const Result res = fsQuickWrite(path, out, needed);
	fcramFree(out);
	return res;
}
