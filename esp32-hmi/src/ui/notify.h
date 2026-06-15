// =====================================================================
//  Notify -- shared confirmation / toast / pick-prompt overlays.
//
//  One reusable building block for the review-feedback "show a
//  confirmation" items (3, 4, 7, 8, 16). Lives on lv_layer_top() like
//  anomaly_modal / locate_overlay. LVGL-task only: every function must
//  be called on the LVGL task.
//
//    toast()        -- brief auto-dismissing banner, no buttons. Used for
//                      load/pick success and refresh-complete cues.
//    confirm()      -- title + message + Confirm/Cancel. Used to gate a
//                      destructive action (inventory unload) behind an
//                      explicit second step.
//    pick_prompt_*  -- interactive "remove the reel from slot N" modal
//                      that the hardware-removal path flips to a success
//                      confirmation and auto-closes.
// =====================================================================
#pragma once

namespace ui {

enum class NotifyMood { Info, Success, Warn, Error };

// Build the (hidden) overlays. Call once from ui::init().
void notify_init();

// ---- Toast --------------------------------------------------------------
// Floating top-centre banner, no backdrop (doesn't block touch), auto-hides
// after `ms` (<=0 => 2000). A new toast replaces any visible one.
void toast(const char* message, NotifyMood mood = NotifyMood::Success, int ms = 2000);

// ---- Confirm ------------------------------------------------------------
struct ConfirmOpts {
    const char* title         = "Confirm";
    const char* message       = "";
    const char* confirm_label = "Confirm";
    const char* cancel_label  = "Cancel";
    bool        danger        = false;          // confirm button styled red
    void      (*on_confirm)(void*) = nullptr;
    void*       user          = nullptr;
};
void confirm(const ConfirmOpts& opts);

// ---- Pick prompt (View-screen pick, review item 16) ---------------------
// Open a modal telling the operator to pull the reel in `slot_num`. The
// caller arms the pick-out separately (lights the slot). on_cancel fires if
// the operator dismisses before pulling the reel. When the hardware removal
// lands, call pick_prompt_done(slot_num): the modal flips to a brief
// "Picked" confirmation and auto-closes.
void pick_prompt_open(int slot_num, void (*on_cancel)(void*), void* user);
void pick_prompt_done(int slot_num);
bool pick_prompt_active(int slot_num);

} // namespace ui
