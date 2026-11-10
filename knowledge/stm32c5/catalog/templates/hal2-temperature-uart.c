// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#include "main.h"
#include "mx_basic_stdio_app.h"
#include "{{adc_header}}"
#include <stdio.h>

static void fail(const char *phase)
{
    printf("BKPT error=%s\r\n", phase);
    for (;;) { HAL_Delay(1000U); }
}

int main(void)
{
    if (mx_system_init() != SYSTEM_OK || mx_basic_stdio_init() != SYSTEM_OK) {
        return -1;
    }
    hal_adc_handle_t *adc = {{adc_init}}();
    if (adc == NULL) { fail("adc_init"); }
    uint32_t adc_hz = HAL_RCC_ADCDAC_GetKernelClkFreq();
    /* Sampling constraint is injected from a reviewed datasheet fact, not inferred from SVD. */
    if (adc_hz == 0U || (uint64_t){{adc_sample_cycles}}U * 1000000U <
        (uint64_t){{temperature_min_sample_us}}U * adc_hz) {
        fail("sampling_time_below_datasheet_minimum");
    }
    if (HAL_ADC_Start(adc) != HAL_OK || HAL_ADC_Calibrate(adc) != HAL_OK) {
        fail("adc_start_calibrate");
    }
    HAL_Delay(1U); /* Exceeds the documented sensor buffer startup maximum. */
    uint32_t count = 0U;
    uint32_t next = HAL_GetTick();
    for (;;) {
        if (HAL_ADC_REG_StartConv(adc) != HAL_OK || HAL_ADC_REG_PollForConv(adc, 10U) != HAL_OK) {
            fail("temperature_conversion");
        }
        uint32_t raw_temperature = (uint32_t)HAL_ADC_REG_ReadConversionData(adc);
        /* One-rank discontinuous sequencing prevents overrun while polling each channel. */
        if (HAL_ADC_REG_TrigNextConv(adc) != HAL_OK || HAL_ADC_REG_PollForConv(adc, 10U) != HAL_OK) {
            fail("vrefint_conversion");
        }
        uint32_t raw_reference = (uint32_t)HAL_ADC_REG_ReadConversionData(adc);
        if (HAL_ADC_REG_StopConv(adc) != HAL_OK || raw_reference == 0U || raw_reference > 4095U || raw_temperature > 4095U) {
            fail("invalid_adc_data");
        }
        uint32_t reference_mv = HAL_ADC_CALC_VREFANALOG_VOLTAGE(raw_reference, HAL_ADC_RESOLUTION_12_BIT);
        int32_t temperature_c = HAL_ADC_CALC_TEMPERATURE(reference_mv, raw_temperature, HAL_ADC_RESOLUTION_12_BIT);
        printf("BKPT sample=%lu temperature_c=%ld vref_mv=%lu raw_temp=%lu raw_vref=%lu adc_hz=%lu\r\n",
               (unsigned long)count++, (long)temperature_c, (unsigned long)reference_mv,
               (unsigned long)raw_temperature, (unsigned long)raw_reference, (unsigned long)adc_hz);
        HAL_GPIO_WritePin({{led_pin}}_PORT, {{led_pin}}_PIN, {{led_pin}}_ACTIVE_STATE);
        HAL_Delay(50U);
        HAL_GPIO_WritePin({{led_pin}}_PORT, {{led_pin}}_PIN, {{led_pin}}_INACTIVE_STATE);
        while ((uint32_t)(HAL_GetTick() - next) < {{period_ms}}U) { }
        next += {{period_ms}}U;
    }
}
