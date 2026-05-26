// =====================================================================
//  Cross-thread dispatch into the LVGL task.
//
//  LVGL is not thread-safe: every call into lv_* APIs must run on the
//  one task that drives lv_timer_handler (in our project, the `lvgl`
//  task pinned to APP_CPU). Code running on the RS485 POLL task, the
//  WiFi task, the SD writer task, etc. must NOT touch LVGL directly.
//
//  This helper wraps lv_async_call: queue a callback to run on the
//  next LVGL refresh tick. The callback receives a single user
//  pointer; everything else has to be captured by the caller (usually
//  by stashing into static state or a small heap-allocated struct
//  freed by the callback).
//
//  Cost: a single linked-list insert under LVGL's mutex. Roughly free.
// =====================================================================
#pragma once

#include <lvgl.h>

namespace ui {

using LvglAsyncFn = void (*)(void* user);

// Queue `fn(user)` to run on the LVGL task. Safe to call from any
// task / ISR-context-clear-of-FreeRTOS-restrictions. If LVGL hasn't
// been initialised yet, the call is silently dropped.
inline void dispatch_on_lvgl(LvglAsyncFn fn, void* user = nullptr) {
    if (!fn) return;
    lv_async_call(reinterpret_cast<lv_async_cb_t>(fn), user);
}

} // namespace ui
