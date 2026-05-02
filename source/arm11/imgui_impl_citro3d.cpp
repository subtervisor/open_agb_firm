#include "arm11/imgui_impl_citro3d.h"

#include <types.h>

extern "C" {
#include <citrine3d.h>
#include <c2d/bcfnt.h>
#include <arm11/allocator/fcram.h>
#include <drivers/cache.h>
#include <debug.h>
#include <fs.h>
}

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "imgui_impl_c3d_shbin.h"

namespace {

constexpr std::size_t kInitialVertices = 4 * 1024;
constexpr std::size_t kInitialIndices  = 8 * 1024;
constexpr float kFontScale = 0.5f;
constexpr const char* kSystemFontPath = "sdmc:/3ds/sysfont.bcfnt";
constexpr const char* kUnifontPath = "sdmc:/3ds/unifont.bcfnt";

struct BackendData {
	DVLB_s* shader = nullptr;
	shaderProgram_s program{};
	C3D_AttrInfo attr{};
	int projection_loc = 0;

	C3D_Mtx projection_top{};
	C3D_Mtx projection_bot{};

	C3D_Tex* font_textures = nullptr;
	unsigned font_texture_count = 0;
	CFNT_s* font = nullptr;
	ImVector<ImWchar> font_ranges;

	ImDrawVert* vertices = nullptr;
	std::size_t vertex_capacity = 0;
	ImDrawIdx* indices = nullptr;
	std::size_t index_capacity = 0;
	uint16_t* triangle_sheets = nullptr;
	std::size_t triangle_capacity = 0;

	unsigned bound_scissor[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
	ImDrawVert* bound_vertices = nullptr;
	C3D_Tex* bound_texture = nullptr;
	uint8_t bound_env = 0;
};

BackendData* get_backend_data()
{
	return ImGui::GetCurrentContext()
	    ? static_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData)
	    : nullptr;
}

void* gpu_alloc(std::size_t size)
{
	return fcramMemAlign(size, 0x80);
}

void gpu_free(void* ptr)
{
	if (ptr) fcramFree(ptr);
}

unsigned clamp_unsigned(float value, unsigned max)
{
	if (value <= 0.0f) return 0;
	if (value >= static_cast<float>(max)) return max;
	return static_cast<unsigned>(value);
}

void setup_font_texenv()
{
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
	for (int i = 1; i < 6; ++i)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
}

void setup_solid_texenv()
{
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, 0xffffffff);
	for (int i = 1; i < 6; ++i)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
}

void setup_image_texenv()
{
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	for (int i = 1; i < 6; ++i)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
}

enum TexEnvMode : uint8_t {
	TEX_ENV_NONE,
	TEX_ENV_FONT,
	TEX_ENV_SOLID,
	TEX_ENV_IMAGE,
};

void set_texenv(BackendData* bd, TexEnvMode mode)
{
	if (bd->bound_env == mode) return;
	if (mode == TEX_ENV_FONT)
		setup_font_texenv();
	else if (mode == TEX_ENV_SOLID)
		setup_solid_texenv();
	else if (mode == TEX_ENV_IMAGE)
		setup_image_texenv();
	bd->bound_env = mode;
}

void setup_for_screen(bool top)
{
	BackendData* bd = get_backend_data();
	std::memset(bd->bound_scissor, 0xff, sizeof(bd->bound_scissor));
	bd->bound_vertices = nullptr;
	bd->bound_texture = nullptr;
	bd->bound_env = TEX_ENV_NONE;

	shaderProgramUse(&bd->program);
	C3D_BindProgram(&bd->program);
	C3D_SetAttrInfo(&bd->attr);

	C3D_DepthTest(false, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_CullFace(GPU_CULL_NONE);

	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, bd->projection_loc,
	                 top ? &bd->projection_top : &bd->projection_bot);
}

void bind_vertex_buffer(BackendData* bd)
{
	C3D_BufInfo* buf_info = C3D_GetBufInfo();
	BufInfo_Init(buf_info);
	BufInfo_Add(buf_info, bd->vertices, sizeof(ImDrawVert), 3, 0x210);
	bd->bound_vertices = bd->vertices;
}

void bind_font_sheet(BackendData* bd, unsigned sheet)
{
	if (sheet == bd->font_texture_count) {
		set_texenv(bd, TEX_ENV_SOLID);
		bd->bound_texture = nullptr;
		return;
	}

	C3D_Tex* tex = &bd->font_textures[sheet];
	C3D_TexBind(0, tex);
	bd->bound_texture = tex;
	set_texenv(bd, TEX_ENV_FONT);
}

bool is_font_texture(BackendData* bd, ImTextureID texture_id)
{
	return texture_id == reinterpret_cast<ImTextureID>(bd->font_textures);
}

bool is_null_texture(ImTextureID texture_id)
{
	return texture_id == static_cast<ImTextureID>(0);
}

bool reserve_buffers(BackendData* bd, int total_vtx, int total_idx)
{
	if (total_vtx > static_cast<int>(bd->vertex_capacity)) {
		gpu_free(bd->vertices);
		bd->vertex_capacity = static_cast<std::size_t>(total_vtx + total_vtx / 4 + 256);
		bd->vertices = static_cast<ImDrawVert*>(gpu_alloc(sizeof(ImDrawVert) * bd->vertex_capacity));
		if (!bd->vertices) return false;
	}

	if (total_idx > static_cast<int>(bd->index_capacity)) {
		gpu_free(bd->indices);
		bd->index_capacity = static_cast<std::size_t>(total_idx + total_idx / 4 + 256);
		bd->indices = static_cast<ImDrawIdx*>(gpu_alloc(sizeof(ImDrawIdx) * bd->index_capacity));
		if (!bd->indices) return false;
	}

	const std::size_t triangles = static_cast<std::size_t>((total_idx + 2) / 3);
	if (triangles > bd->triangle_capacity) {
		gpu_free(bd->triangle_sheets);
		bd->triangle_capacity = triangles + triangles / 4 + 64;
		bd->triangle_sheets = static_cast<uint16_t*>(
		    gpu_alloc(sizeof(uint16_t) * bd->triangle_capacity));
		if (!bd->triangle_sheets) return false;
	}

	return true;
}

bool copy_draw_data(BackendData* bd, ImDrawData* draw_data)
{
	std::size_t vertex_base = 0;
	std::size_t index_base = 0;
	for (int list_i = 0; list_i < draw_data->CmdListsCount; ++list_i) {
		const ImDrawList* list = draw_data->CmdLists[list_i];
		std::memcpy(&bd->vertices[vertex_base], list->VtxBuffer.Data,
		            sizeof(ImDrawVert) * list->VtxBuffer.Size);

		for (const ImDrawCmd& cmd : list->CmdBuffer) {
			const ImDrawIdx* src = &list->IdxBuffer.Data[cmd.IdxOffset];
			ImDrawIdx* dst = &bd->indices[index_base + cmd.IdxOffset];
			const std::size_t base = vertex_base + cmd.VtxOffset;
			for (unsigned k = 0; k < cmd.ElemCount; ++k) {
				const std::size_t idx = base + src[k];
				if (idx > 0xffffu)
					return false;
				dst[k] = static_cast<ImDrawIdx>(idx);
			}
		}

		vertex_base += list->VtxBuffer.Size;
		index_base += list->IdxBuffer.Size;
	}
	return true;
}

CFNT_s* load_system_font()
{
	FHandle f;
	if (fOpen(&f, kSystemFontPath, FA_OPEN_EXISTING | FA_READ) != RES_OK &&
	    fOpen(&f, kUnifontPath, FA_OPEN_EXISTING | FA_READ) != RES_OK)
		panicMsg("Failed to load system font or fallback");

	const u32 size = fSize(f);
	CFNT_s* font = static_cast<CFNT_s*>(fcramMemAlign(size, 0x80));
	if (!font) {
		fClose(f);
		panicMsg("Failed to allocate memory for system font");
	}

	u32 bytes_read = 0;
	Result res = RES_OK;
	if (size)
		res = fRead(f, font, size, &bytes_read);
	fClose(f);
	if (res != RES_OK || bytes_read != size) {
		fcramFree(font);
		panicMsg("Failed to read system font");
	}

	cleanDCacheRange(font, size);
	fontFixPointers(font);
	return font;
}

int compare_codepoints(const void* lhs, const void* rhs)
{
	const ImWchar a = *static_cast<const ImWchar*>(lhs);
	const ImWchar b = *static_cast<const ImWchar*>(rhs);
	return (a > b) - (a < b);
}

void append_codepoint(ImVector<ImWchar>* codepoints, uint32_t codepoint)
{
	if (codepoint == 0 || codepoint > 0xffffu)
		return;
	codepoints->push_back(static_cast<ImWchar>(codepoint));
}

void enumerate_font_codepoints(FINF_s* font_info, ImVector<ImWchar>* codepoints)
{
	for (CMAP_s* cmap = font_info->cmap; cmap; cmap = cmap->next) {
		const uint32_t begin = cmap->codeBegin;
		const uint32_t end = cmap->codeEnd;

		if (cmap->mappingMethod == CMAP_TYPE_DIRECT) {
			for (uint32_t codepoint = begin; codepoint <= end; ++codepoint) {
				const uint32_t glyph_index =
				    cmap->indexOffset + (codepoint - begin);
				if (glyph_index == 0xffffu)
					break;
				append_codepoint(codepoints, codepoint);
			}
			continue;
		}

		if (cmap->mappingMethod == CMAP_TYPE_TABLE) {
			for (uint32_t codepoint = begin; codepoint <= end; ++codepoint) {
				if (cmap->indexTable[codepoint - begin] != 0xffffu)
					append_codepoint(codepoints, codepoint);
			}
			continue;
		}

		if (cmap->mappingMethod == CMAP_TYPE_SCAN) {
			for (uint16_t i = 0; i < cmap->nScanEntries; ++i) {
				if (cmap->scanEntries[i].glyphIndex != 0xffffu)
					append_codepoint(codepoints, cmap->scanEntries[i].code);
			}
		}
	}
}

void sort_unique_codepoints(ImVector<ImWchar>* codepoints)
{
	if (codepoints->Size <= 1)
		return;

	qsort(codepoints->Data, codepoints->Size, sizeof(ImWchar), compare_codepoints);

	int unique_count = 1;
	for (int i = 1; i < codepoints->Size; ++i) {
		if ((*codepoints)[i] == (*codepoints)[unique_count - 1])
			continue;
		(*codepoints)[unique_count++] = (*codepoints)[i];
	}
	codepoints->shrink(unique_count);
}

void build_font_ranges(FINF_s* font_info, ImVector<ImWchar>* codepoints,
                       ImVector<ImWchar>* ranges)
{
	codepoints->clear();
	ranges->clear();

	enumerate_font_codepoints(font_info, codepoints);
	sort_unique_codepoints(codepoints);
	if (codepoints->empty())
		panicMsg("System font has no mapped glyphs");

	int range_count = 1;
	for (int i = 1; i < codepoints->Size; ++i) {
		if ((*codepoints)[i] != static_cast<ImWchar>((*codepoints)[i - 1] + 1))
			++range_count;
	}
	ranges->reserve(range_count * 2 + 1);

	ImWchar start = (*codepoints)[0];
	ImWchar prev = start;
	for (int i = 1; i < codepoints->Size; ++i) {
		const ImWchar codepoint = (*codepoints)[i];
		if (codepoint != static_cast<ImWchar>(prev + 1)) {
			ranges->push_back(start);
			ranges->push_back(prev);
			start = codepoint;
		}
		prev = codepoint;
	}
	ranges->push_back(start);
	ranges->push_back(prev);
	ranges->push_back(0);
}

void build_font(BackendData* bd)
{
	CFNT_s* font = load_system_font();
	bd->font = font;
	FINF_s* font_info = fontGetInfo(font);
	TGLP_s* glyph_info = fontGetGlyphInfo(font);

	bd->font_texture_count = static_cast<unsigned>(glyph_info->nSheets);
	bd->font_textures = static_cast<C3D_Tex*>(
	    fcramAlloc(sizeof(C3D_Tex) * bd->font_texture_count));
	if (!bd->font_textures)
		panicMsg("Failed to allocate system font texture descriptors");
	std::memset(bd->font_textures, 0, sizeof(C3D_Tex) * bd->font_texture_count);

	for (int i = 0; i < glyph_info->nSheets; ++i) {
		C3D_Tex* tex = &bd->font_textures[i];
		tex->data = fontGetGlyphSheetTex(font, i);
		tex->fmt = static_cast<GPU_TEXCOLOR>(glyph_info->sheetFmt);
		tex->size = glyph_info->sheetSize;
		tex->width = glyph_info->sheetWidth;
		tex->height = glyph_info->sheetHeight;
		tex->param = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) |
		             GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
		             GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_BORDER) |
		             GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_BORDER);
		tex->border = 0;
		tex->lodParam = 0;
	}

	ImFontAtlas* atlas = ImGui::GetIO().Fonts;
	atlas->Clear();
	atlas->TexWidth = glyph_info->sheetWidth;
	atlas->TexHeight = glyph_info->sheetHeight * glyph_info->nSheets;
	atlas->TexUvScale = ImVec2(1.0f / atlas->TexWidth, 1.0f / atlas->TexHeight);
	atlas->TexUvWhitePixel =
	    ImVec2(0.5f / 8.0f, static_cast<float>(glyph_info->nSheets) + 0.5f / 8.0f);
	atlas->TexPixelsAlpha8 = static_cast<unsigned char*>(IM_ALLOC(1));

	ImVector<ImWchar> codepoints;
	build_font_ranges(font_info, &codepoints, &bd->font_ranges);

	ImFontConfig cfg;
	cfg.FontData = nullptr;
	cfg.FontDataSize = 0;
	cfg.FontDataOwnedByAtlas = false;
	cfg.SizePixels = 14.0f;
	cfg.OversampleH = 1;
	cfg.OversampleV = 1;
	cfg.PixelSnapH = true;
	cfg.GlyphOffset = ImVec2(0.0f, font_info->ascent);
	cfg.GlyphRanges = bd->font_ranges.Data;
	cfg.EllipsisChar = '.';
	std::memset(cfg.Name, 0, sizeof(cfg.Name));

	ImFont* im_font = IM_NEW(ImFont);
	cfg.DstFont = im_font;
	atlas->ConfigData.push_back(cfg);
	atlas->Fonts.push_back(im_font);
	atlas->SetTexID(reinterpret_cast<ImTextureID>(bd->font_textures));

	im_font->FallbackAdvanceX = font_info->defaultWidth.charWidth;
	im_font->FontSize = font_info->lineFeed;
	im_font->ContainerAtlas = atlas;
	im_font->ConfigData = &atlas->ConfigData[0];
	im_font->ConfigDataCount = 1;
	im_font->FallbackChar = '?';
	im_font->EllipsisChar = '.';
	im_font->Scale = kFontScale;
	im_font->Ascent = font_info->ascent;
	im_font->Descent = 0.0f;

	auto add_glyph = [&](ImWchar c) {
		const int glyph_index = fontGlyphIndexFromCodePoint(font, c);
		if (glyph_index < 0 || glyph_index >= 0xffff)
			return;
		fontGlyphPos_s pos;
		fontCalcGlyphPos(&pos, font, glyph_index,
		                 GLYPH_POS_CALC_VTXCOORD | GLYPH_POS_AT_BASELINE,
		                 1.0f, 1.0f);
		im_font->AddGlyph(
		    &cfg, c,
		    pos.vtxcoord.left,
		    pos.vtxcoord.top + font_info->ascent,
		    pos.vtxcoord.right,
		    pos.vtxcoord.bottom + font_info->ascent,
		    pos.texcoord.left,
		    static_cast<float>(pos.sheetIndex) + pos.texcoord.top,
		    pos.texcoord.right,
		    static_cast<float>(pos.sheetIndex) + pos.texcoord.bottom,
		    pos.xAdvance);
	};

	for (int i = 0; i < codepoints.Size; ++i)
		add_glyph(codepoints[i]);
	codepoints.clear();

	im_font->BuildLookupTable();
	atlas->TexReady = true;
}

void preprocess_font_draws(BackendData* bd, ImDrawData* draw_data)
{
	std::size_t vertex_base = 0;
	std::size_t index_base = 0;

	for (int list_i = 0; list_i < draw_data->CmdListsCount; ++list_i) {
		const ImDrawList* list = draw_data->CmdLists[list_i];
		for (const ImDrawCmd& cmd : list->CmdBuffer) {
			const ImTextureID texture_id = cmd.GetTexID();
			const bool font_texture = is_font_texture(bd, texture_id);
			const bool null_texture = is_null_texture(texture_id);
			if (!font_texture && !null_texture)
				continue;

			const ImDrawVert* src_vtx = &list->VtxBuffer.Data[cmd.VtxOffset];
			ImDrawVert* dst_vtx = &bd->vertices[vertex_base + cmd.VtxOffset];
			const ImDrawIdx* src_idx = &list->IdxBuffer.Data[cmd.IdxOffset];

			for (unsigned k = 0; k + 2 < cmd.ElemCount; k += 3) {
				const ImDrawIdx a = src_idx[k + 0];
				const ImDrawIdx b = src_idx[k + 1];
				const ImDrawIdx c = src_idx[k + 2];
				unsigned sheet = bd->font_texture_count;
				if (font_texture) {
					const float ab = src_vtx[a].uv.y < src_vtx[b].uv.y ? src_vtx[a].uv.y : src_vtx[b].uv.y;
					const float lo = ab < src_vtx[c].uv.y ? ab : src_vtx[c].uv.y;
					sheet = static_cast<unsigned>(lo);
					if (sheet > bd->font_texture_count)
						sheet = 0;
				}
				bd->triangle_sheets[(index_base + cmd.IdxOffset + k) / 3] =
				    static_cast<uint16_t>(sheet);
			}

			for (unsigned k = 0; k < cmd.ElemCount; ++k) {
				float whole = 0.0f;
				dst_vtx[src_idx[k]].uv.y = std::modf(dst_vtx[src_idx[k]].uv.y, &whole);
			}
		}
		vertex_base += list->VtxBuffer.Size;
		index_base += list->IdxBuffer.Size;
	}
}

void draw_font_cmd(BackendData* bd, const ImDrawCmd& cmd, std::size_t absolute_index)
{
	const std::size_t base_tri = absolute_index / 3;
	unsigned start = 0;
	unsigned sheet = bd->triangle_sheets[base_tri];
	bind_font_sheet(bd, sheet);

	for (unsigned k = 3; k < cmd.ElemCount; k += 3) {
		const unsigned next_sheet = bd->triangle_sheets[base_tri + k / 3];
		if (next_sheet == sheet) continue;

		C3D_DrawElements(GPU_TRIANGLES, k - start, C3D_UNSIGNED_SHORT,
		                 &bd->indices[absolute_index + start]);
		sheet = next_sheet;
		start = k;
		bind_font_sheet(bd, sheet);
	}

	C3D_DrawElements(GPU_TRIANGLES, cmd.ElemCount - start, C3D_UNSIGNED_SHORT,
	                 &bd->indices[absolute_index + start]);
}

void draw_screen(ImDrawData* draw_data, C3D_RenderTarget* target, bool top)
{
	BackendData* bd = get_backend_data();
	C3D_FrameDrawOn(target);
	setup_for_screen(top);
	bind_vertex_buffer(bd);

	const unsigned width = static_cast<unsigned>(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
	const unsigned height = static_cast<unsigned>(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
	const ImVec2 clip_off = draw_data->DisplayPos;
	const ImVec2 clip_scale = draw_data->FramebufferScale;

	std::size_t vertex_base = 0;
	std::size_t index_base = 0;

	for (int list_i = 0; list_i < draw_data->CmdListsCount; ++list_i) {
		const ImDrawList* list = draw_data->CmdLists[list_i];
		for (const ImDrawCmd& cmd : list->CmdBuffer) {
			if (cmd.UserCallback) {
				if (cmd.UserCallback == ImDrawCallback_ResetRenderState) {
					setup_for_screen(top);
					bind_vertex_buffer(bd);
				} else {
					cmd.UserCallback(list, &cmd);
				}
				continue;
			}

			ImVec4 clip;
			clip.x = (cmd.ClipRect.x - clip_off.x) * clip_scale.x;
			clip.y = (cmd.ClipRect.y - clip_off.y) * clip_scale.y;
			clip.z = (cmd.ClipRect.z - clip_off.x) * clip_scale.x;
			clip.w = (cmd.ClipRect.w - clip_off.y) * clip_scale.y;

			if (clip.x >= width || clip.y >= height || clip.z < 0.0f || clip.w < 0.0f)
				continue;
			if (clip.x < 0.0f) clip.x = 0.0f;
			if (clip.y < 0.0f) clip.y = 0.0f;
			if (clip.z > width) clip.z = width;
			if (clip.w > height) clip.w = height;

			if (top) {
				if (clip.y > height * 0.5f) continue;
				const unsigned x1 = clamp_unsigned(height * 0.5f - clip.w, height / 2);
				const unsigned y1 = clamp_unsigned(width - clip.z, width);
				const unsigned x2 = clamp_unsigned(height * 0.5f - clip.y, height / 2);
				const unsigned y2 = clamp_unsigned(width - clip.x, width);
				if (bd->bound_scissor[0] != x1 || bd->bound_scissor[1] != y1 ||
				    bd->bound_scissor[2] != x2 || bd->bound_scissor[3] != y2) {
					bd->bound_scissor[0] = x1;
					bd->bound_scissor[1] = y1;
					bd->bound_scissor[2] = x2;
					bd->bound_scissor[3] = y2;
					C3D_SetScissor(GPU_SCISSOR_NORMAL, x1, y1, x2, y2);
				}
			} else {
				if (clip.w < height * 0.5f) continue;
				if (clip.z < width * 0.1f) continue;
				if (clip.x > width * 0.9f) continue;
				const unsigned x1 = clamp_unsigned(height - clip.w, height / 2);
				const unsigned y1 = clamp_unsigned(width * 0.9f - clip.z, width * 8 / 10);
				const unsigned x2 = clamp_unsigned(height - clip.y, height / 2);
				const unsigned y2 = clamp_unsigned(width * 0.9f - clip.x, width * 8 / 10);
				if (bd->bound_scissor[0] != x1 || bd->bound_scissor[1] != y1 ||
				    bd->bound_scissor[2] != x2 || bd->bound_scissor[3] != y2) {
					bd->bound_scissor[0] = x1;
					bd->bound_scissor[1] = y1;
					bd->bound_scissor[2] = x2;
					bd->bound_scissor[3] = y2;
					C3D_SetScissor(GPU_SCISSOR_NORMAL, x1, y1, x2, y2);
				}
			}

			const ImTextureID texture_id = cmd.GetTexID();
			const bool font_cmd = is_font_texture(bd, texture_id);
			if (font_cmd) {
				draw_font_cmd(bd, cmd, index_base + cmd.IdxOffset);
			} else if (is_null_texture(texture_id)) {
				bind_font_sheet(bd, bd->font_texture_count);
				C3D_DrawElements(GPU_TRIANGLES, cmd.ElemCount, C3D_UNSIGNED_SHORT,
				                 &bd->indices[index_base + cmd.IdxOffset]);
			} else {
				C3D_Tex* tex = reinterpret_cast<C3D_Tex*>(texture_id);
				if (tex != bd->bound_texture) {
					C3D_TexBind(0, tex);
					bd->bound_texture = tex;
				}
				set_texenv(bd, TEX_ENV_IMAGE);
				C3D_DrawElements(GPU_TRIANGLES, cmd.ElemCount, C3D_UNSIGNED_SHORT,
				                 &bd->indices[index_base + cmd.IdxOffset]);
			}
		}

		vertex_base += list->VtxBuffer.Size;
		index_base += list->IdxBuffer.Size;
	}
}

} // namespace

IMGUI_IMPL_API bool ImGui_ImplCitro3D_Init(bool)
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.BackendRendererUserData != nullptr)
		return false;

	void* bd_mem = fcramAlloc(sizeof(BackendData));
	if (!bd_mem)
		return false;

	BackendData* bd = new (bd_mem) BackendData();
	io.BackendRendererUserData = bd;
	io.BackendRendererName = "imgui_impl_citrine3d";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

	bd->shader = DVLB_ParseFile(
	    const_cast<u32*>(reinterpret_cast<const u32*>(imgui_impl_c3d_shbin)),
	    imgui_impl_c3d_shbin_size);
	shaderProgramInit(&bd->program);
	shaderProgramSetVsh(&bd->program, &bd->shader->DVLE[0]);
	bd->projection_loc =
	    shaderInstanceGetUniformLocation(bd->program.vertexShader, "projection");

	AttrInfo_Init(&bd->attr);
	AttrInfo_AddLoader(&bd->attr, 0, GPU_FLOAT, 2);
	AttrInfo_AddLoader(&bd->attr, 1, GPU_FLOAT, 2);
	AttrInfo_AddLoader(&bd->attr, 2, GPU_UNSIGNED_BYTE, 4);

	bd->vertex_capacity = kInitialVertices;
	bd->index_capacity = kInitialIndices;
	bd->triangle_capacity = kInitialIndices / 3;
	bd->vertices = static_cast<ImDrawVert*>(gpu_alloc(sizeof(ImDrawVert) * bd->vertex_capacity));
	bd->indices = static_cast<ImDrawIdx*>(gpu_alloc(sizeof(ImDrawIdx) * bd->index_capacity));
	bd->triangle_sheets = static_cast<uint16_t*>(
	    gpu_alloc(sizeof(uint16_t) * bd->triangle_capacity));
	if (!bd->vertices || !bd->indices || !bd->triangle_sheets) {
		gpu_free(bd->vertices);
		gpu_free(bd->indices);
		gpu_free(bd->triangle_sheets);
		io.BackendRendererUserData = nullptr;
		bd->~BackendData();
		fcramFree(bd);
		return false;
	}

	build_font(bd);
	return true;
}

IMGUI_IMPL_API void ImGui_ImplCitro3D_Shutdown()
{
	BackendData* bd = get_backend_data();
	if (!bd) return;

	gpu_free(bd->vertices);
	gpu_free(bd->indices);
	gpu_free(bd->triangle_sheets);
	if (bd->font_textures)
		fcramFree(bd->font_textures);
	if (bd->font)
		fcramFree(bd->font);
	shaderProgramFree(&bd->program);
	DVLB_Free(bd->shader);

	ImGui::GetIO().BackendRendererUserData = nullptr;
	bd->~BackendData();
	fcramFree(bd);
}

IMGUI_IMPL_API void ImGui_ImplCitro3D_NewFrame()
{
}

IMGUI_IMPL_API void ImGui_ImplCitro3D_RenderDrawData(ImDrawData* draw_data,
                                                     void* t_top,
                                                     void* t_bot)
{
	if (!draw_data || draw_data->CmdListsCount <= 0)
		return;

	BackendData* bd = get_backend_data();
	const unsigned width = static_cast<unsigned>(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
	const unsigned height = static_cast<unsigned>(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
	if (width == 0 || height == 0)
		return;

	if (!reserve_buffers(bd, draw_data->TotalVtxCount, draw_data->TotalIdxCount))
		return;

	Mtx_OrthoTilt(&bd->projection_top,
	              0.0f, draw_data->DisplaySize.x,
	              draw_data->DisplaySize.y * 0.5f, 0.0f,
	              -1.0f, 1.0f, false);
	Mtx_OrthoTilt(&bd->projection_bot,
	              draw_data->DisplaySize.x * 0.1f, draw_data->DisplaySize.x * 0.9f,
	              draw_data->DisplaySize.y, draw_data->DisplaySize.y * 0.5f,
	              -1.0f, 1.0f, false);

	if (!copy_draw_data(bd, draw_data))
		return;

	preprocess_font_draws(bd, draw_data);
	cleanDCacheRange(bd->vertices, sizeof(ImDrawVert) * draw_data->TotalVtxCount);
	cleanDCacheRange(bd->indices, sizeof(ImDrawIdx) * draw_data->TotalIdxCount);

	draw_screen(draw_data, static_cast<C3D_RenderTarget*>(t_top), true);
	draw_screen(draw_data, static_cast<C3D_RenderTarget*>(t_bot), false);
}

IMGUI_IMPL_API void ImGui_ImplCitro3D_LoadFontTextures()
{
}
