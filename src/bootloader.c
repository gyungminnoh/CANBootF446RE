#include "bootloader.h"
#include "boot_config.h"
#include "boot_request.h"
#include "can_if.h"
#include "flash_if.h"
#include "stm32f4xx_hal.h"

extern CAN_HandleTypeDef hcan1;

typedef struct {
    uint32_t magic;
    uint32_t app_size;
    uint32_t app_crc;
    uint32_t state;
} app_meta_t;

typedef enum {
    APP_STATE_INVALID = 0x00,
    APP_STATE_VALID = 0x01,
    APP_STATE_UPDATE_IN_PROGRESS = 0x02,
    APP_STATE_METADATA_ERASED = 0x03,
    APP_STATE_CRC_FAILED = 0x04,
} app_state_t;

static uint32_t current_write_addr = APP_START_ADDR;
static uint16_t expected_seq;
static uint8_t expected_fast_seq;
static bool app_crc_valid;
static app_state_t runtime_app_state = APP_STATE_INVALID;

static uint32_t read_le32(const uint8_t *data)
{
    return ((uint32_t)data[0]) |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint8_t checksum8(const uint8_t *data, uint32_t len)
{
    uint8_t sum = 0;

    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + data[i]);
    }

    return sum;
}

static uint32_t crc32_update(uint32_t crc, uint8_t data)
{
    crc ^= data;

    for (uint32_t i = 0; i < 8U; i++) {
        if ((crc & 1U) != 0U) {
            crc = (crc >> 1) ^ 0xEDB88320UL;
        } else {
            crc >>= 1;
        }
    }

    return crc;
}

static uint32_t crc32_calculate(uint32_t address, uint32_t size)
{
    uint32_t crc = 0xFFFFFFFFUL;
    const uint8_t *ptr = (const uint8_t *)address;

    for (uint32_t i = 0; i < size; i++) {
        crc = crc32_update(crc, ptr[i]);
    }

    return crc ^ 0xFFFFFFFFUL;
}

static bool is_fast_seq_data_id(uint32_t id)
{
    return (id & CAN_HOST_SEQ_DATA_MASK_ID) == CAN_HOST_SEQ_DATA_BASE_ID;
}

static void bootloader_handle_fast_data_packet(const can_packet_t *pkt, bool check_seq)
{
    uint32_t word0;
    uint32_t word1;
    uint8_t seq;

    if (pkt->dlc != 8U) {
        (void)can_if_send_nack(CMD_DATA, BOOT_ERR_INVALID_LENGTH);
        return;
    }

    if (check_seq) {
        seq = (uint8_t)(pkt->id & 0xFFU);
        if (seq != expected_fast_seq) {
            (void)can_if_send_nack(CMD_DATA, BOOT_ERR_SEQUENCE);
            return;
        }
    }

    if (((current_write_addr & 0x3UL) != 0UL) ||
        !flash_if_is_in_app_region(current_write_addr, 8U)) {
        (void)can_if_send_nack(CMD_DATA, BOOT_ERR_INVALID_ADDRESS);
        return;
    }

    word0 = read_le32(&pkt->data[0]);
    word1 = read_le32(&pkt->data[4]);

    if (!flash_if_write_word(current_write_addr, word0) ||
        !flash_if_write_word(current_write_addr + sizeof(uint32_t), word1)) {
        (void)can_if_send_nack(CMD_DATA, BOOT_ERR_FLASH_WRITE);
        return;
    }

    current_write_addr += 8U;
    if (check_seq) {
        expected_fast_seq++;
    }
    app_crc_valid = false;
    (void)can_if_send_ack(CMD_DATA);
}

static bool is_valid_sram_address(uint32_t address)
{
    return (address >= SRAM_START_ADDR) && (address <= SRAM_END_ADDR);
}

static bool is_valid_reset_handler(uint32_t address)
{
    uint32_t normalized = address & ~1UL;

    return ((address & 1UL) != 0UL) &&
           flash_if_is_valid_app_address(normalized);
}

static bool bootloader_is_valid_metadata(void)
{
    const app_meta_t *meta = (const app_meta_t *)APP_META_ADDR;
    uint32_t actual_crc;

    if (meta->magic != APP_META_MAGIC) {
        return false;
    }

    if (meta->state != APP_META_STATE_VALID) {
        return false;
    }

    if (!flash_if_is_in_app_region(APP_START_ADDR, meta->app_size)) {
        return false;
    }

    actual_crc = crc32_calculate(APP_START_ADDR, meta->app_size);
    return actual_crc == meta->app_crc;
}

static app_state_t bootloader_get_app_state(void)
{
    const app_meta_t *meta = (const app_meta_t *)APP_META_ADDR;

    if (runtime_app_state == APP_STATE_UPDATE_IN_PROGRESS) {
        return APP_STATE_UPDATE_IN_PROGRESS;
    }

    if (runtime_app_state == APP_STATE_CRC_FAILED) {
        return APP_STATE_CRC_FAILED;
    }

    if ((meta->magic == APP_META_STATE_ERASED) &&
        (meta->app_size == APP_META_STATE_ERASED) &&
        (meta->app_crc == APP_META_STATE_ERASED) &&
        (meta->state == APP_META_STATE_ERASED)) {
        return APP_STATE_METADATA_ERASED;
    }

    if (bootloader_is_valid_app()) {
        return APP_STATE_VALID;
    }

    return APP_STATE_INVALID;
}

void bootloader_main(void)
{
    bool forced_update = boot_request_is_set();
    bool app_valid;

    if (forced_update) {
        boot_request_clear();
    }

    app_valid = bootloader_is_valid_app();

    if (!forced_update && app_valid &&
        !bootloader_wait_update_request(BOOT_UPDATE_WAIT_MS)) {
        bootloader_jump_to_app();
    }

    (void)can_if_start();
    bootloader_loop();
}

void bootloader_loop(void)
{
    while (1) {
        can_packet_t pkt;

        if (can_if_receive_poll(&pkt)) {
            bootloader_handle_packet(&pkt);
        }
    }
}

void bootloader_handle_packet(const can_packet_t *pkt)
{
    uint8_t cmd;

    if (pkt == 0) {
        return;
    }

    if (pkt->id == CAN_HOST_DATA_ID) {
        bootloader_handle_fast_data_packet(pkt, false);
        return;
    }

    if (is_fast_seq_data_id(pkt->id)) {
        bootloader_handle_fast_data_packet(pkt, true);
        return;
    }

    if ((pkt->id != CAN_HOST_CMD_ID) || (pkt->dlc == 0U)) {
        return;
    }

    cmd = pkt->data[0];

    switch (cmd) {
    case CMD_PING: {
        uint8_t response[8] = {
            CAN_ACK,
            CMD_PING,
            BOOTLOADER_VERSION_MAJOR,
            BOOTLOADER_VERSION_MINOR,
            0, 0, 0, 0
        };
        (void)can_if_send_response(response, sizeof(response));
        break;
    }

    case CMD_INFO: {
        uint8_t page = 0U;
        uint8_t response[8] = {
            CAN_ACK,
            CMD_INFO,
            0, 0, 0, 0, 0, 0
        };

        if (pkt->dlc >= 2U) {
            page = pkt->data[1];
        }

        response[2] = page;

        switch (page) {
        case 0:
            response[3] = BOOTLOADER_VERSION_MAJOR;
            response[4] = BOOTLOADER_VERSION_MINOR;
            response[5] = 0x01U;
            response[6] = 0x00U;
            response[7] = 0x01U;
            (void)can_if_send_response(response, sizeof(response));
            break;

        case 1:
            response[3] = (uint8_t)(APP_START_ADDR & 0xFFU);
            response[4] = (uint8_t)((APP_START_ADDR >> 8) & 0xFFU);
            response[5] = (uint8_t)((APP_START_ADDR >> 16) & 0xFFU);
            response[6] = (uint8_t)((APP_START_ADDR >> 24) & 0xFFU);
            (void)can_if_send_response(response, sizeof(response));
            break;

        case 2:
            response[3] = (uint8_t)(APP_FLASH_END_ADDR & 0xFFU);
            response[4] = (uint8_t)((APP_FLASH_END_ADDR >> 8) & 0xFFU);
            response[5] = (uint8_t)((APP_FLASH_END_ADDR >> 16) & 0xFFU);
            response[6] = (uint8_t)((APP_FLASH_END_ADDR >> 24) & 0xFFU);
            (void)can_if_send_response(response, sizeof(response));
            break;

        case 3:
            response[3] = (uint8_t)(APP_META_ADDR & 0xFFU);
            response[4] = (uint8_t)((APP_META_ADDR >> 8) & 0xFFU);
            response[5] = (uint8_t)((APP_META_ADDR >> 16) & 0xFFU);
            response[6] = (uint8_t)((APP_META_ADDR >> 24) & 0xFFU);
            (void)can_if_send_response(response, sizeof(response));
            break;

        case 4: {
            const app_meta_t *meta = (const app_meta_t *)APP_META_ADDR;

            response[3] = (uint8_t)bootloader_get_app_state();
            response[4] = (uint8_t)(meta->app_size & 0xFFU);
            response[5] = (uint8_t)((meta->app_size >> 8) & 0xFFU);
            response[6] = (uint8_t)((meta->app_size >> 16) & 0xFFU);
            response[7] = (uint8_t)((meta->app_size >> 24) & 0xFFU);
            (void)can_if_send_response(response, sizeof(response));
            break;
        }

        case 5: {
            const app_meta_t *meta = (const app_meta_t *)APP_META_ADDR;

            response[3] = (uint8_t)(meta->app_crc & 0xFFU);
            response[4] = (uint8_t)((meta->app_crc >> 8) & 0xFFU);
            response[5] = (uint8_t)((meta->app_crc >> 16) & 0xFFU);
            response[6] = (uint8_t)((meta->app_crc >> 24) & 0xFFU);
            (void)can_if_send_response(response, sizeof(response));
            break;
        }

        default:
            (void)can_if_send_nack(CMD_INFO, BOOT_ERR_INVALID_ADDRESS);
            break;
        }
        break;
    }

    case CMD_ERASE:
        if (flash_if_erase_app()) {
            current_write_addr = APP_START_ADDR;
            expected_seq = 0;
            expected_fast_seq = 0;
            app_crc_valid = false;
            runtime_app_state = APP_STATE_UPDATE_IN_PROGRESS;
            (void)can_if_send_ack(CMD_ERASE);
        } else {
            (void)can_if_send_nack(CMD_ERASE, BOOT_ERR_ERASE_FAILED);
        }
        break;

    case CMD_SET_ADDR: {
        uint32_t address;

        if (pkt->dlc < 5U) {
            (void)can_if_send_nack(CMD_SET_ADDR, BOOT_ERR_INVALID_LENGTH);
            break;
        }

        address = read_le32(&pkt->data[1]);

        if (((address & 0x3UL) != 0UL) ||
            !flash_if_is_in_app_region(address, sizeof(uint32_t))) {
            (void)can_if_send_nack(CMD_SET_ADDR, BOOT_ERR_INVALID_ADDRESS);
            break;
        }

        current_write_addr = address;
        expected_seq = 0;
        expected_fast_seq = 0;
        app_crc_valid = false;
        (void)can_if_send_ack(CMD_SET_ADDR);
        break;
    }

    case CMD_DATA: {
        uint16_t seq;
        uint32_t word;
        uint8_t expected_checksum;

        if (pkt->dlc != 8U) {
            (void)can_if_send_nack(CMD_DATA, BOOT_ERR_INVALID_LENGTH);
            break;
        }

        expected_checksum = checksum8(pkt->data, 7U);
        if (pkt->data[7] != expected_checksum) {
            (void)can_if_send_nack(CMD_DATA, BOOT_ERR_CHECKSUM);
            break;
        }

        seq = (uint16_t)pkt->data[1] | ((uint16_t)pkt->data[2] << 8);
        if (seq != expected_seq) {
            (void)can_if_send_nack(CMD_DATA, BOOT_ERR_SEQUENCE);
            break;
        }

        word = read_le32(&pkt->data[3]);

        if (!flash_if_write_word(current_write_addr, word)) {
            (void)can_if_send_nack(CMD_DATA, BOOT_ERR_FLASH_WRITE);
            break;
        }

        current_write_addr += sizeof(uint32_t);
        expected_seq++;
        app_crc_valid = false;
        (void)can_if_send_ack(CMD_DATA);
        break;
    }

    case CMD_CRC: {
        uint32_t app_size;
        uint32_t expected_crc;
        uint32_t actual_crc;

        if (pkt->dlc < 5U) {
            (void)can_if_send_nack(CMD_CRC, BOOT_ERR_INVALID_LENGTH);
            break;
        }

        app_size = current_write_addr - APP_START_ADDR;
        expected_crc = read_le32(&pkt->data[1]);

        if (!flash_if_is_in_app_region(APP_START_ADDR, app_size)) {
            (void)can_if_send_nack(CMD_CRC, BOOT_ERR_INVALID_ADDRESS);
            break;
        }

        actual_crc = crc32_calculate(APP_START_ADDR, app_size);
        if (actual_crc != expected_crc) {
            app_crc_valid = false;
            runtime_app_state = APP_STATE_CRC_FAILED;
            (void)can_if_send_nack(CMD_CRC, BOOT_ERR_CRC_MISMATCH);
            break;
        }

        app_crc_valid = flash_if_write_metadata(app_size, actual_crc, APP_META_STATE_VALID);
        if (!app_crc_valid) {
            (void)can_if_send_nack(CMD_CRC, BOOT_ERR_METADATA_WRITE);
            break;
        }
        runtime_app_state = APP_STATE_VALID;
        (void)can_if_send_ack(CMD_CRC);
        break;
    }

    case CMD_BOOT:
        if (bootloader_is_valid_app() && (app_crc_valid || bootloader_is_valid_metadata())) {
            (void)can_if_send_ack(CMD_BOOT);
            HAL_Delay(20);
            bootloader_jump_to_app();
        } else {
            (void)can_if_send_nack(CMD_BOOT, BOOT_ERR_APP_INVALID);
        }
        break;

    case CMD_RESET:
        (void)can_if_send_ack(CMD_RESET);
        HAL_Delay(20);
        NVIC_SystemReset();
        break;

    default:
        (void)can_if_send_nack(cmd, BOOT_ERR_UNKNOWN_CMD);
        break;
    }
}

bool bootloader_wait_update_request(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    (void)can_if_start();

    while ((HAL_GetTick() - start) < timeout_ms) {
        can_packet_t pkt;

        if (can_if_receive_poll(&pkt) &&
            (pkt.id == CAN_HOST_CMD_ID) &&
            (pkt.dlc > 0U) &&
            (pkt.data[0] == CMD_PING)) {
            bootloader_handle_packet(&pkt);
            return true;
        }
    }

    return false;
}

bool bootloader_is_valid_app(void)
{
    uint32_t app_stack = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_START_ADDR + 4UL);

    return is_valid_sram_address(app_stack) &&
           is_valid_reset_handler(app_reset) &&
           bootloader_is_valid_metadata();
}

void bootloader_jump_to_app(void)
{
    uint32_t app_stack = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_START_ADDR + 4UL);

    __disable_irq();

    HAL_CAN_DeInit(&hcan1);
    HAL_DeInit();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    for (uint32_t i = 0; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    SCB->VTOR = APP_START_ADDR;

    __DSB();
    __ISB();

    __enable_irq();

    __ASM volatile(
        "msr msp, %0\n"
        "bx %1\n"
        :
        : "r"(app_stack), "r"(app_reset)
        :);

    while (1) {
    }
}
