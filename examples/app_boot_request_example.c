#include "stm32f4xx_hal.h"
#include <stdint.h>

#define APP_START_ADDR          0x08010000UL
#define SRAM_END_ADDR           0x20020000UL
#define BOOT_MAGIC_ADDR         (SRAM_END_ADDR - 4UL)
#define BOOT_MAGIC_VALUE        0xB007C0DEUL

static void request_bootloader_mode(void)
{
    volatile uint32_t *boot_magic = (volatile uint32_t *)BOOT_MAGIC_ADDR;

    __disable_irq();
    *boot_magic = BOOT_MAGIC_VALUE;
    __DSB();
    __ISB();
    NVIC_SystemReset();
}

int main(void)
{
    SCB->VTOR = APP_START_ADDR;

    HAL_Init();
    SystemClock_Config();

    while (1) {
        /*
         * Replace this condition with the app's CAN update command handler.
         * The app should only request bootloader mode and reset.
         */
        if (0) {
            request_bootloader_mode();
        }
    }
}
