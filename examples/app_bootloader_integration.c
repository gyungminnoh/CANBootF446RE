/*
 * Example code to integrate an application with the CAN bootloader.
 *
 * Copy the relevant pieces into the real app:
 * - SCB->VTOR relocation in main()
 * - boot_request.c/h and boot_config.h
 * - CAN command handler for CMD_PING and CMD_RESET on the node-specific ID
 */

#include "boot_config.h"
#include "boot_request.h"
#include "can_if.h"
#include "stm32f4xx_hal.h"

extern CAN_HandleTypeDef hcan1;

static void app_send_can_response(const uint8_t *data, uint8_t len)
{
    CAN_TxHeaderTypeDef tx_header = {0};
    uint32_t mailbox;

    tx_header.StdId = CAN_BOOT_RESP_ID;
    tx_header.IDE = CAN_ID_STD;
    tx_header.RTR = CAN_RTR_DATA;
    tx_header.DLC = len;
    tx_header.TransmitGlobalTime = DISABLE;

    (void)HAL_CAN_AddTxMessage(&hcan1, &tx_header, (uint8_t *)data, &mailbox);
}

static void app_request_bootloader_reset(void)
{
    boot_request_set();

    __DSB();
    __ISB();

    HAL_Delay(20);
    NVIC_SystemReset();
}

void app_handle_bootloader_can_command(const CAN_RxHeaderTypeDef *rx_header,
                                       const uint8_t data[8])
{
    if ((rx_header == 0) || (data == 0)) {
        return;
    }

    if ((rx_header->IDE != CAN_ID_STD) ||
        (rx_header->RTR != CAN_RTR_DATA) ||
        (rx_header->StdId != CAN_HOST_CMD_ID) ||
        (rx_header->DLC == 0U)) {
        return;
    }

    switch (data[0]) {
    case CMD_PING: {
        uint8_t response[8] = {
            CAN_ACK,
            CMD_PING,
            0xA0U,
            0x01U,
            0, 0, 0, 0
        };
        app_send_can_response(response, sizeof(response));
        break;
    }

    case CMD_RESET: {
        uint8_t response[8] = {
            CAN_ACK,
            CMD_RESET,
            1U,
            0, 0, 0, 0, 0
        };
        app_send_can_response(response, sizeof(response));
        app_request_bootloader_reset();
        break;
    }

    default: {
        uint8_t response[8] = {
            CAN_NACK,
            data[0],
            BOOT_ERR_UNKNOWN_CMD,
            0, 0, 0, 0, 0
        };
        app_send_can_response(response, sizeof(response));
        break;
    }
    }
}

int main(void)
{
    SCB->VTOR = APP_START_ADDR;

    HAL_Init();
    SystemClock_Config();

    /*
     * Initialize GPIO/CAN/peripherals here.
     * The app linker script must place FLASH ORIGIN at 0x08010000.
     */

    while (1) {
        /*
         * Poll CAN or call app_handle_bootloader_can_command() from the
         * app's CAN RX callback/ring-buffer consumer.
         */
    }
}
