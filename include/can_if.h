#ifndef CAN_IF_H
#define CAN_IF_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
} can_packet_t;

typedef enum {
    CMD_PING     = 0x01,
    CMD_INFO     = 0x02,
    CMD_ERASE    = 0x03,
    CMD_SET_ADDR = 0x04,
    CMD_DATA     = 0x05,
    CMD_CRC      = 0x06,
    CMD_BOOT     = 0x07,
    CMD_RESET    = 0x08,
} boot_cmd_t;

typedef enum {
    BOOT_ERR_UNKNOWN_CMD       = 0x01,
    BOOT_ERR_ERASE_FAILED      = 0x02,
    BOOT_ERR_INVALID_LENGTH    = 0x03,
    BOOT_ERR_INVALID_ADDRESS   = 0x04,
    BOOT_ERR_CHECKSUM          = 0x05,
    BOOT_ERR_SEQUENCE          = 0x06,
    BOOT_ERR_FLASH_WRITE       = 0x07,
    BOOT_ERR_APP_INVALID       = 0x08,
    BOOT_ERR_CRC_MISMATCH      = 0x09,
    BOOT_ERR_METADATA_WRITE    = 0x0A,
} boot_error_t;

#define CAN_ACK  0x79U
#define CAN_NACK 0x1FU

bool can_if_start(void);
bool can_if_send_ack(uint8_t cmd);
bool can_if_send_nack(uint8_t cmd, uint8_t error_code);
bool can_if_send_response(const uint8_t *data, uint8_t len);
bool can_if_receive_poll(can_packet_t *pkt);

#endif
