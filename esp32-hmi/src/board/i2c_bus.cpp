#include "board/i2c_bus.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace i2c_bus {

static SemaphoreHandle_t s_mtx = nullptr;

void init() {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
}

Lock::Lock()  { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
Lock::~Lock() { if (s_mtx) xSemaphoreGive(s_mtx); }

} // namespace i2c_bus
