#ifndef FLASH_IF_H
#define FLASH_IF_H

#include <stdbool.h>
#include <stdint.h>

bool flash_if_is_valid_app_address(uint32_t address);
bool flash_if_is_in_app_region(uint32_t address, uint32_t size);
uint32_t flash_if_get_sector(uint32_t address);
bool flash_if_unlock(void);
bool flash_if_lock(void);
bool flash_if_erase_app(void);
bool flash_if_write_word(uint32_t address, uint32_t data);
bool flash_if_write_metadata(uint32_t app_size, uint32_t app_crc, uint32_t version);

#endif
