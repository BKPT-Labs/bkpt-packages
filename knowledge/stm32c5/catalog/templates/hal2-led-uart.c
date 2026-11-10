// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#include "main.h"
#include "mx_basic_stdio_app.h"
#include <stdio.h>

int main(void)
{
    if (mx_system_init() != SYSTEM_OK || mx_basic_stdio_init() != SYSTEM_OK) {
        return -1;
    }
    uint32_t count = 0;
    uint32_t next = HAL_GetTick();
    for (;;) {
        HAL_GPIO_WritePin({{led_pin}}_PORT, {{led_pin}}_PIN, {{led_pin}}_ACTIVE_STATE);
        printf("BKPT tick=%lu period_ms={{period_ms}}\r\n", (unsigned long)count++);
        HAL_Delay(50U);
        HAL_GPIO_WritePin({{led_pin}}_PORT, {{led_pin}}_PIN, {{led_pin}}_INACTIVE_STATE);
        while ((uint32_t)(HAL_GetTick() - next) < {{period_ms}}U) { }
        next += {{period_ms}}U;
    }
}
