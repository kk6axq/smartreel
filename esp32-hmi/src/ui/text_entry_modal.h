// =====================================================================
//  Generic text-entry modal.
//
//  Full-screen overlay on lv_layer_top with a single text field + LVGL
//  keyboard. One instance, opened with text_entry_open(); the caller
//  passes a title, an initial value, and a SaveFn that runs when the
//  user taps Save (or the keyboard's check button).
//
//  Used by the network config screen to edit the InvenTree URL and
//  token. Pattern mirrors ui/wifi_password_modal.* but lifts the
//  password/SSID specifics out so the same widget can edit anything.
// =====================================================================
#pragma once

namespace ui {

using TextEntrySaveFn = void (*)(const char* new_value);

// One-time build. Hidden by default. Call after lvgl init.
void text_entry_modal_init();

struct TextEntryOpts {
    const char*       title;          // header, e.g. "InvenTree URL"
    const char*       initial;        // pre-filled value (may be nullptr)
    const char*       placeholder;    // ghost text when empty
    bool              password_mode;  // mask input as bullets
    int               max_len;        // 0 = no limit (LVGL default)
    TextEntrySaveFn   on_save;        // required; receives the entered text
};

void text_entry_modal_open(const TextEntryOpts& opts);
void text_entry_modal_close();

} // namespace ui
