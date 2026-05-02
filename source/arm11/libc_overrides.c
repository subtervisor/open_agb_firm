/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2024 derrek, profi200
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 */

// Replacements for newlib's float-parse / generic-scanf entry points so the
// linker resolves them to our tiny stubs instead of pulling the full libc
// stdio chain. ImGui calls atof from RoundScalarWithFormatT (slider value
// snap-to-displayed-precision) and DataTypeApplyFromText (number text-edit),
// and sscanf from the auto-registered Window/Table settings handlers — all
// of which would otherwise drag in __ssvfscanf_r / _strtod_l / _dtoa_r etc.
// (~50 KB of .text) for code paths we never execute at runtime.
//
// Saves are routed through util.c's str2double, which matches the precision
// of our INI parser (3 decimal places, no exponents). The settings-handler
// sscanf calls never execute at runtime — io.IniFilename is nullptr so the
// imgui INI loader never runs — so a no-op is correct.

#include "types.h"
#include "util.h" // str2double

#include <stddef.h>
#include <stdarg.h>


// Standard wrappers. Declared here (not via <stdlib.h> / <stdio.h>) so we
// don't pick up newlib's __nonnull / format-attribute decorations and so the
// linker sees these object-file symbols before searching libc.a.
double atof(const char *s);
double strtod(const char *s, char **endptr);
float  strtof(const char *s, char **endptr);
int    sscanf(const char *s, const char *fmt, ...);


double atof(const char *s)
{
	return s ? str2double(s) : 0.0;
}

double strtod(const char *s, char **endptr)
{
	if(endptr != NULL)
	{
		// Best-effort end pointer: skip leading whitespace then the run of
		// characters str2double understands. Doesn't try to be precise about
		// where the parse really stopped — just enough that callers using
		// "did anything parse?" via (endptr == str) get the right answer.
		const char *p = s;
		if(p != NULL)
		{
			while(*p == ' ' || *p == '\t') p++;
			if(*p == '+' || *p == '-') p++;
			while(*p >= '0' && *p <= '9') p++;
			if(*p == '.') p++;
			while(*p >= '0' && *p <= '9') p++;
		}
		*endptr = (char*)(p ? p : s);
	}
	return s ? str2double(s) : 0.0;
}

float strtof(const char *s, char **endptr)
{
	return (float)strtod(s, endptr);
}

// imgui's WindowSettingsHandler / TableSettingsHandler call sscanf to parse
// INI lines. With io.IniFilename = nullptr those handlers never run — but
// the linker can't tell, so the call sites are kept and pull in 30+ KB of
// __ssvfscanf_r / strtoll / etc. Returning -1 ("no items matched") keeps
// the handlers' early-out paths happy if they ever DO run.
int sscanf(const char *s, const char *fmt, ...)
{
	(void)s;
	(void)fmt;
	return -1;
}
