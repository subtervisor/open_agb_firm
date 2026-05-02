#include "imgui_internal.h"

#include <cstdarg>
#include <cstddef>
#include <cstring>

#include <types.h>

extern "C" {
#include <arm11/fmt.h>
}

namespace {

bool has_float_format(const char* fmt)
{
	for (const char* p = fmt; *p; ++p) {
		if (*p != '%') continue;
		++p;
		if (*p == '%') continue;
		while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') ++p;
		while (*p >= '0' && *p <= '9') ++p;
		if (*p == '.') {
			++p;
			while (*p >= '0' && *p <= '9') ++p;
		}
		while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 't' || *p == 'j') ++p;
		if (*p == 'f' || *p == 'F' || *p == 'g' || *p == 'G')
			return true;
		if (*p == 0)
			break;
	}
	return false;
}

struct Out {
	char* buf;
	size_t cap;
	size_t len;
};

struct Scratch {
	char float_tmp[48];
	char frac_tmp[12];
	char fmt_tmp[32];
	char text_tmp[192];
};

void out_char(Out* out, char c)
{
	if (out->buf && out->cap > 0 && out->len + 1 < out->cap)
		out->buf[out->len] = c;
	++out->len;
}

void out_text(Out* out, const char* text, size_t len)
{
	for (size_t i = 0; i < len; ++i)
		out_char(out, text[i]);
}

void out_cstr(Out* out, const char* text)
{
	out_text(out, text, std::strlen(text));
}

void finish(Out* out)
{
	if (!out->buf || out->cap == 0)
		return;
	const size_t pos = out->len < out->cap ? out->len : out->cap - 1;
	out->buf[pos] = '\0';
	if (out->len >= out->cap)
		out->len = out->cap - 1;
}

int pow10_int(int n)
{
	int v = 1;
	while (n-- > 0) v *= 10;
	return v;
}

void format_float_to_temp(Scratch* scratch, double value, int precision, bool trim)
{
	char* tmp = scratch->float_tmp;
	const size_t tmp_size = sizeof(scratch->float_tmp);
	if (tmp_size == 0)
		return;
	if (precision < 0)
		precision = 6;
	if (precision > 6)
		precision = 6;

	char* p = tmp;
	char* end = tmp + tmp_size - 1;
	if (value < 0.0) {
		if (p < end) *p++ = '-';
		value = -value;
	}

	const int scale = pow10_int(precision);
	unsigned whole = static_cast<unsigned>(value);
	unsigned frac = static_cast<unsigned>((value - static_cast<double>(whole)) * scale + 0.5);
	if (frac >= static_cast<unsigned>(scale)) {
		++whole;
		frac -= scale;
	}

	p += ee_snprintf(p, static_cast<u32>(end - p + 1), "%u", whole);
	if (precision > 0 && p < end) {
		*p++ = '.';
		ee_snprintf(scratch->frac_tmp, sizeof(scratch->frac_tmp), "%0*u", precision, frac);
		for (int i = 0; i < precision && p < end; ++i)
			*p++ = scratch->frac_tmp[i];
		if (trim) {
			while (p > tmp && p[-1] == '0') --p;
			if (p > tmp && p[-1] == '.') --p;
		}
	}
	*p = '\0';
}

void out_padded(Out* out, const char* text, int width, bool left, bool zero)
{
	const int len = static_cast<int>(std::strlen(text));
	const int pad = width > len ? width - len : 0;
	if (!left) {
		for (int i = 0; i < pad; ++i)
			out_char(out, zero ? '0' : ' ');
	}
	out_text(out, text, len);
	if (left) {
		for (int i = 0; i < pad; ++i)
			out_char(out, ' ');
	}
}

int format_with_float_support(char* buf, size_t buf_size, const char* fmt, va_list args)
{
	Scratch* scratch = static_cast<Scratch*>(IM_ALLOC(sizeof(Scratch)));
	if (!scratch)
		return 0;

	Out out{buf, buf_size, 0};
	for (const char* p = fmt; *p; ++p) {
		if (*p != '%') {
			out_char(&out, *p);
			continue;
		}

		const char* spec_start = p;
		++p;
		if (*p == '%') {
			out_char(&out, '%');
			continue;
		}

		bool left = false;
		bool zero = false;
		while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
			if (*p == '-') left = true;
			if (*p == '0') zero = true;
			++p;
		}

		int width = 0;
		while (*p >= '0' && *p <= '9')
			width = width * 10 + (*p++ - '0');

		int precision = -1;
		if (*p == '.') {
			++p;
			precision = 0;
			while (*p >= '0' && *p <= '9')
				precision = precision * 10 + (*p++ - '0');
		}

		int long_count = 0;
		int short_count = 0;
		while (*p == 'l' || *p == 'h') {
			if (*p == 'l') ++long_count;
			if (*p == 'h') ++short_count;
			++p;
		}

		const char spec = *p;
		if (spec == 'f' || spec == 'F' || spec == 'g' || spec == 'G') {
			format_float_to_temp(scratch, va_arg(args, double), precision,
			                     spec == 'g' || spec == 'G');
			out_padded(&out, scratch->float_tmp, width, left, zero);
			continue;
		}

		const size_t spec_len = static_cast<size_t>(p - spec_start + 1);
		if (spec_len >= sizeof(scratch->fmt_tmp)) {
			out_char(&out, '%');
			continue;
		}
		std::memcpy(scratch->fmt_tmp, spec_start, spec_len);
		scratch->fmt_tmp[spec_len] = '\0';

		switch (spec) {
			case 's':
				ee_snprintf(scratch->text_tmp, sizeof(scratch->text_tmp),
				            scratch->fmt_tmp, va_arg(args, const char*));
				break;
			case 'c':
				ee_snprintf(scratch->text_tmp, sizeof(scratch->text_tmp),
				            scratch->fmt_tmp, va_arg(args, int));
				break;
			case 'p':
				ee_snprintf(scratch->text_tmp, sizeof(scratch->text_tmp),
				            scratch->fmt_tmp, va_arg(args, void*));
				break;
			case 'd':
			case 'i':
				if (long_count >= 2)
					ee_snprintf(scratch->text_tmp, sizeof(scratch->text_tmp),
					            scratch->fmt_tmp, va_arg(args, long long));
				else
					ee_snprintf(scratch->text_tmp, sizeof(scratch->text_tmp),
					            scratch->fmt_tmp, va_arg(args, int));
				break;
			case 'u':
			case 'x':
			case 'X':
				if (long_count >= 2)
					ee_snprintf(scratch->text_tmp, sizeof(scratch->text_tmp),
					            scratch->fmt_tmp, va_arg(args, unsigned long long));
				else
					ee_snprintf(scratch->text_tmp, sizeof(scratch->text_tmp),
					            scratch->fmt_tmp, va_arg(args, unsigned int));
				break;
			default:
				scratch->text_tmp[0] = '%';
				scratch->text_tmp[1] = spec;
				scratch->text_tmp[2] = '\0';
				break;
		}
		(void)short_count;
		out_cstr(&out, scratch->text_tmp);
	}

	finish(&out);
	const int ret = static_cast<int>(out.len);
	IM_FREE(scratch);
	return ret;
}

} // namespace

int ImFormatString(char* buf, size_t buf_size, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	const int ret = ImFormatStringV(buf, buf_size, fmt, args);
	va_end(args);
	return ret;
}

int ImFormatStringV(char* buf, size_t buf_size, const char* fmt, va_list args)
{
	if (!has_float_format(fmt)) {
		if (!buf || buf_size == 0) {
			constexpr u32 kScratchSize = 1024;
			char* tmp = static_cast<char*>(IM_ALLOC(kScratchSize));
			if (!tmp)
				return 0;
			const int ret = static_cast<int>(ee_vsnprintf(tmp, kScratchSize, fmt, args));
			IM_FREE(tmp);
			return ret;
		}
		return static_cast<int>(ee_vsnprintf(buf, static_cast<u32>(buf_size), fmt, args));
	}

	return format_with_float_support(buf, buf_size, fmt, args);
}
