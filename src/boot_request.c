#include "boot_request.h"
#include "boot_config.h"
#include "stm32f4xx_hal.h"

static void boot_request_enable_backup_domain(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_RTC_ENABLE();
}

void boot_request_set(void)
{
    boot_request_enable_backup_domain();
    RTC->BKP0R = BOOT_MAGIC_VALUE;
}

void boot_request_clear(void)
{
    boot_request_enable_backup_domain();
    RTC->BKP0R = 0UL;
}

bool boot_request_is_set(void)
{
    boot_request_enable_backup_domain();
    return RTC->BKP0R == BOOT_MAGIC_VALUE;
}
