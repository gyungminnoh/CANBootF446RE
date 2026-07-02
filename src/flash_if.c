#include "flash_if.h"
#include "boot_config.h"
#include "stm32f4xx_hal.h"

bool flash_if_is_valid_app_address(uint32_t address)
{
    return (address >= APP_START_ADDR) && (address < APP_FLASH_END_ADDR);
}

bool flash_if_is_in_app_region(uint32_t address, uint32_t size)
{
    if (size == 0U) {
        return false;
    }

    if (address < APP_START_ADDR) {
        return false;
    }

    if (address >= APP_FLASH_END_ADDR) {
        return false;
    }

    return size <= (APP_FLASH_END_ADDR - address);
}

uint32_t flash_if_get_sector(uint32_t address)
{
    if (address < 0x08004000UL) {
        return FLASH_SECTOR_0;
    }
    if (address < 0x08008000UL) {
        return FLASH_SECTOR_1;
    }
    if (address < 0x0800C000UL) {
        return FLASH_SECTOR_2;
    }
    if (address < 0x08010000UL) {
        return FLASH_SECTOR_3;
    }
    if (address < 0x08020000UL) {
        return FLASH_SECTOR_4;
    }
    if (address < 0x08040000UL) {
        return FLASH_SECTOR_5;
    }
    if (address < 0x08060000UL) {
        return FLASH_SECTOR_6;
    }
    return FLASH_SECTOR_7;
}

bool flash_if_unlock(void)
{
    return HAL_FLASH_Unlock() == HAL_OK;
}

bool flash_if_lock(void)
{
    return HAL_FLASH_Lock() == HAL_OK;
}

bool flash_if_erase_app(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0;
    HAL_StatusTypeDef status;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase.Sector = FLASH_SECTOR_4;
    erase.NbSectors = FLASH_SECTOR_7 - FLASH_SECTOR_4 + 1U;

    if (!flash_if_is_in_app_region(APP_START_ADDR, APP_FLASH_END_ADDR - APP_START_ADDR)) {
        return false;
    }

    if (!flash_if_unlock()) {
        return false;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    (void)flash_if_lock();

    return (status == HAL_OK) && (sector_error == 0xFFFFFFFFUL);
}

bool flash_if_write_word(uint32_t address, uint32_t data)
{
    HAL_StatusTypeDef status;

    if ((address & 0x3UL) != 0UL) {
        return false;
    }

    if (!flash_if_is_in_app_region(address, sizeof(uint32_t))) {
        return false;
    }

    if (!flash_if_unlock()) {
        return false;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, data);
    (void)flash_if_lock();

    if (status != HAL_OK) {
        return false;
    }

    return *(volatile uint32_t *)address == data;
}

bool flash_if_write_metadata(uint32_t app_size, uint32_t app_crc, uint32_t version)
{
    const uint32_t metadata[4] = {
        APP_META_MAGIC,
        app_size,
        app_crc,
        version
    };

    for (uint32_t i = 0; i < 4U; i++) {
        uint32_t address = APP_META_ADDR + (i * sizeof(uint32_t));

        if (!flash_if_unlock()) {
            return false;
        }

        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                               FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, metadata[i]) != HAL_OK) {
            (void)flash_if_lock();
            return false;
        }

        (void)flash_if_lock();

        if (*(volatile uint32_t *)address != metadata[i]) {
            return false;
        }
    }

    return true;
}
