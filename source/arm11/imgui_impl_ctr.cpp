#include "arm11/imgui_impl_ctr.h"

#include <types.h>

extern "C" {
#include <arm11/drivers/hid.h>
}

namespace {

void add_button(ImGuiIO& io, u32 held, u32 down, u32 up, u32 in, ImGuiKey out)
{
	if (up & in)
		io.AddKeyEvent(out, false);
	else if (down & in)
		io.AddKeyEvent(out, true);
	else if (!(held & in))
		io.AddKeyEvent(out, false);
}

float analog_value(s16 value, bool positive)
{
	constexpr float kDeadZone = 400.0f;
	constexpr float kMax = 2048.0f;
	float f = static_cast<float>(positive ? value : -value);
	if (f <= kDeadZone) return 0.0f;
	f = (f - kDeadZone) / (kMax - kDeadZone);
	if (f > 1.0f) f = 1.0f;
	return f;
}

void process_input(ImGuiIO& io)
{
	const u32 held = hidKeysHeld();
	const u32 down = hidKeysDown();
	const u32 up = hidKeysUp();

	add_button(io, held, down, up, KEY_A,      ImGuiKey_GamepadFaceRight);
	add_button(io, held, down, up, KEY_B,      ImGuiKey_GamepadFaceDown);
	// KEY_X (GamepadFaceUp) is intentionally NOT forwarded to imgui. ImGui
	// treats it as ImGuiKey_NavGamepadInput and routes it to the same
	// "activate widget" path as A — useful only on text/number-input
	// widgets to enter "type a value" mode. We have no software keyboard
	// (so text entry is a no-op anyway), so binding X just creates a
	// second "activate" button that doubles every press of A and conflicts
	// with the X+DUP/X+DDOWN backlight hotkeys in open_agb_firm.c. Wire
	// this back up if a soft keyboard ever lands.
	add_button(io, held, down, up, KEY_Y,      ImGuiKey_GamepadFaceLeft);
	add_button(io, held, down, up, KEY_L,      ImGuiKey_GamepadL1);
	add_button(io, held, down, up, KEY_ZL,     ImGuiKey_GamepadL2);
	add_button(io, held, down, up, KEY_R,      ImGuiKey_GamepadR1);
	add_button(io, held, down, up, KEY_ZR,     ImGuiKey_GamepadR2);
	add_button(io, held, down, up, KEY_DUP,    ImGuiKey_GamepadDpadUp);
	add_button(io, held, down, up, KEY_DRIGHT, ImGuiKey_GamepadDpadRight);
	add_button(io, held, down, up, KEY_DDOWN,  ImGuiKey_GamepadDpadDown);
	add_button(io, held, down, up, KEY_DLEFT,  ImGuiKey_GamepadDpadLeft);
	add_button(io, held, down, up, KEY_SELECT, ImGuiKey_GamepadBack);
	add_button(io, held, down, up, KEY_START,  ImGuiKey_GamepadStart);

	const CpadPos* cpad = hidGetCpadPosPtr();
	const float left = analog_value(cpad->x, false);
	const float right = analog_value(cpad->x, true);
	const float up_axis = analog_value(cpad->y, true);
	const float down_axis = analog_value(cpad->y, false);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft,  left > 0.0f, left);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, right > 0.0f, right);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp,    up_axis > 0.0f, up_axis);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown,  down_axis > 0.0f, down_axis);
}

// Touch state machine (ported from oaf-configurator-firm/source/nk_input_libn3ds.cpp).
// Distinguishes tap from vertical-scroll from horizontal-drag so a stylus
// drag inside a list scrolls the window instead of clicking through every
// item the cursor passes over.
//
//   IDLE      no contact, cursor parked off-screen.
//   PENDING   contact, total movement < threshold; no input fed yet (so
//             ImGui doesn't activate the widget under the touch-down).
//   SCROLL_V  vertical drag confirmed; we feed scroll-wheel deltas via
//             g_pendingScrollY (consumed by the file browser render),
//             cursor stays parked.
//   DRAG_H    horizontal drag confirmed; pass through as motion + sticky
//             LMB so slider widgets adjust normally.
enum class TouchState : u8
{
	IDLE,
	PENDING,
	SCROLL_V,
	DRAG_H,
};

constexpr int   kTouchThreshold = 4;     // px before a press is reclassified.
constexpr float kScrollGain     = 1.0f;  // wheel ticks per finger pixel; small.

float g_pendingScrollY = 0.0f;           // see ImGui_ImplCtr_ConsumePendingScrollY().

void process_touch(ImGuiIO& io)
{
	static TouchState state = TouchState::IDLE;
	static int start_x = 0, start_y = 0;
	static int last_x  = 0, last_y  = 0;

	const u32 held = hidKeysHeld();
	const bool down = (held & KEY_TOUCH) != 0;

	if (down)
	{
		const TouchPos* p = hidGetTouchPosPtr();
		// Translate bottom-screen-local coords into the imgui canvas.
		const int x = static_cast<int>(p->x) + 40;
		const int y = static_cast<int>(p->y) + 240;

		if (state == TouchState::IDLE)
		{
			state   = TouchState::PENDING;
			start_x = last_x = x;
			start_y = last_y = y;
			// Park cursor off-screen for now so widgets don't see hover.
			io.AddMousePosEvent(-10.0f, -10.0f);
			return;
		}

		const int dx_total = x - start_x;
		const int dy_total = y - start_y;
		const int abs_dx   = dx_total < 0 ? -dx_total : dx_total;
		const int abs_dy   = dy_total < 0 ? -dy_total : dy_total;
		const int dy_frame = y - last_y;

		if (state == TouchState::PENDING)
		{
			if (abs_dy >= kTouchThreshold && abs_dy >= abs_dx)
			{
				state = TouchState::SCROLL_V;
				// Drag stylus DOWN ↔ list content moves DOWN (revealing
				// items above) ↔ wheel scroll-up (positive Y).
				g_pendingScrollY += static_cast<float>(dy_frame) * kScrollGain;
			}
			else if (abs_dx >= kTouchThreshold)
			{
				state = TouchState::DRAG_H;
				io.AddMousePosEvent(static_cast<float>(start_x), static_cast<float>(start_y));
				io.AddMouseButtonEvent(0, true);
			}
			// else: still pending; hold input.
		}
		else if (state == TouchState::SCROLL_V)
		{
			g_pendingScrollY += static_cast<float>(dy_frame) * kScrollGain;
			io.AddMousePosEvent(-10.0f, -10.0f);
		}
		else // DRAG_H
		{
			io.AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
			// LMB stays sticky.
		}

		last_x = x;
		last_y = y;
	}
	else
	{
		const TouchState prev_state = state;
		state = TouchState::IDLE;

		if (prev_state == TouchState::PENDING)
		{
			// Sub-threshold press → synthesise a tap at the start position.
			io.AddMousePosEvent(static_cast<float>(start_x), static_cast<float>(start_y));
			io.AddMouseButtonEvent(0, true);
			io.AddMouseButtonEvent(0, false);
		}
		else if (prev_state == TouchState::DRAG_H)
		{
			io.AddMouseButtonEvent(0, false);
		}
		else
		{
			// IDLE or SCROLL_V → cursor parked, no button to release.
			io.AddMousePosEvent(-10.0f, -10.0f);
			io.AddMouseButtonEvent(0, false);
		}
	}
}

} // namespace

IMGUI_IMPL_API bool ImGui_ImplCtr_Init()
{
	ImGuiIO& io = ImGui::GetIO();

	io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	io.BackendPlatformName = "imgui_impl_libn3ds";
	io.ConfigNavSwapGamepadButtons = true;
	io.MouseDrawCursor = false;
	io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);

	return true;
}

IMGUI_IMPL_API void ImGui_ImplCtr_Shutdown()
{
}

IMGUI_IMPL_API void ImGui_ImplCtr_NewFrame()
{
	ImGuiIO& io = ImGui::GetIO();
	io.DeltaTime = 1.0f / 60.0f;
	process_input(io);
	process_touch(io);
}

IMGUI_IMPL_API float ImGui_ImplCtr_ConsumePendingScrollY()
{
	const float v = g_pendingScrollY;
	g_pendingScrollY = 0.0f;
	return v;
}
