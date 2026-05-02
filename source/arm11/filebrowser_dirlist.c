/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2021 derrek, profi200
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 */

#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "arm11/filebrowser_dirlist.h"
#include "arm11/allocator/fcram.h"
#include "fs.h"
#include "util.h"


#define DIR_READ_BLOCKS  (10u)


static int dlistCompare(const void *a, const void *b)
{
	const char *entA = *(char**)a;
	const char *entB = *(char**)b;

	// Compare the entry type. Dirs have priority over files.
	if(*entA != *entB) return (int)*entB - *entA;

	// Compare the string.
	int res;
	do
	{
		res = *++entA - *++entB;
	} while(res == 0 && *entA != '\0' && *entB != '\0');

	return res;
}

Result fbScanDir(const char *const path, FbDirList *const dList, const char *const filter)
{
	if(path == NULL || dList == NULL || filter == NULL) return RES_INVALID_ARG;

	FILINFO *const fis = (FILINFO*)fcramAlloc(sizeof(FILINFO) * DIR_READ_BLOCKS);
	if(fis == NULL) return RES_OUT_OF_MEM;

	dList->num = 0;

	Result res;
	DHandle dh;
	if((res = fOpenDir(&dh, path)) == RES_OK)
	{
		u32 read;
		u32 numEntries = 0;
		u32 entBufPos = 0;
		const u32 filterLen = strlen(filter);
		do
		{
			if((res = fReadDir(dh, fis, DIR_READ_BLOCKS, &read)) != RES_OK) break;
			read = (read <= FB_MAX_DIR_ENTRIES - numEntries ? read : FB_MAX_DIR_ENTRIES - numEntries);

			for(u32 i = 0; i < read; i++)
			{
				const char entType = (fis[i].fattrib & AM_DIR ? FB_ENT_TYPE_DIR : FB_ENT_TYPE_FILE);
				const u32 nameLen = strlen(fis[i].fname);
				if(entType == FB_ENT_TYPE_FILE)
				{
					if(filterLen != 0 && (nameLen <= filterLen
					    || strcmp(filter, fis[i].fname + nameLen - filterLen) != 0))
						continue;
					if(fis[i].fname[0] == '.') continue;
				}

				if(entBufPos + nameLen + 2 > FB_MAX_ENT_BUF_SIZE) goto scanEnd;

				char *const entry = &dList->entBuf[entBufPos];
				*entry = entType;
				safeStrcpy(&entry[1], fis[i].fname, 256);
				dList->ptrs[numEntries++] = entry;
				entBufPos += nameLen + 2;
			}
		} while(read == DIR_READ_BLOCKS);

scanEnd:
		dList->num = numEntries;

		fCloseDir(dh);
	}

	fcramFree(fis);

	qsort(dList->ptrs, dList->num, sizeof(char*), dlistCompare);

	return res;
}
