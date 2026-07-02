#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stdbool.h>
#include <stdint.h>

#include "can_if.h"

void bootloader_main(void);
void bootloader_loop(void);
void bootloader_handle_packet(const can_packet_t *pkt);
bool bootloader_wait_update_request(uint32_t timeout_ms);
bool bootloader_is_valid_app(void);
void bootloader_jump_to_app(void);

#endif
