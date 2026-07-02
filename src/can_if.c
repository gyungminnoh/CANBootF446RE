#include "can_if.h"
#include "boot_config.h"
#include "stm32f4xx_hal.h"

extern CAN_HandleTypeDef hcan1;

static bool can_started;

static bool can_if_configure_filter(void)
{
    CAN_FilterTypeDef cmd_filter = {0};
    CAN_FilterTypeDef data_filter = {0};
    CAN_FilterTypeDef seq_data_filter = {0};

    cmd_filter.FilterBank = 0;
    cmd_filter.FilterMode = CAN_FILTERMODE_IDMASK;
    cmd_filter.FilterScale = CAN_FILTERSCALE_32BIT;
    cmd_filter.FilterIdHigh = (uint16_t)(CAN_HOST_CMD_ID << 5);
    cmd_filter.FilterIdLow = 0x0000U;
    cmd_filter.FilterMaskIdHigh = (uint16_t)(0x7FFU << 5);
    cmd_filter.FilterMaskIdLow = 0x0000U;
    cmd_filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    cmd_filter.FilterActivation = ENABLE;
    cmd_filter.SlaveStartFilterBank = 14;

    data_filter = cmd_filter;
    data_filter.FilterBank = 1;
    data_filter.FilterIdHigh = (uint16_t)(CAN_HOST_DATA_ID << 5);

    seq_data_filter = cmd_filter;
    seq_data_filter.FilterBank = 2;
    seq_data_filter.FilterIdHigh = (uint16_t)(CAN_HOST_SEQ_DATA_BASE_ID << 5);
    seq_data_filter.FilterMaskIdHigh = (uint16_t)(CAN_HOST_SEQ_DATA_MASK_ID << 5);

    return (HAL_CAN_ConfigFilter(&hcan1, &cmd_filter) == HAL_OK) &&
           (HAL_CAN_ConfigFilter(&hcan1, &data_filter) == HAL_OK) &&
           (HAL_CAN_ConfigFilter(&hcan1, &seq_data_filter) == HAL_OK);
}

bool can_if_start(void)
{
    if (can_started) {
        return true;
    }

    if (!can_if_configure_filter()) {
        return false;
    }

    if (HAL_CAN_Start(&hcan1) != HAL_OK) {
        return false;
    }

    can_started = true;
    return true;
}

bool can_if_send_ack(uint8_t cmd)
{
    uint8_t data[8] = {CAN_ACK, cmd, 0, 0, 0, 0, 0, 0};
    return can_if_send_response(data, sizeof(data));
}

bool can_if_send_nack(uint8_t cmd, uint8_t error_code)
{
    uint8_t data[8] = {CAN_NACK, cmd, error_code, 0, 0, 0, 0, 0};
    return can_if_send_response(data, sizeof(data));
}

bool can_if_send_response(const uint8_t *data, uint8_t len)
{
    CAN_TxHeaderTypeDef tx_header = {0};
    uint32_t mailbox;

    if ((data == 0) || (len > 8U)) {
        return false;
    }

    tx_header.StdId = CAN_BOOT_RESP_ID;
    tx_header.IDE = CAN_ID_STD;
    tx_header.RTR = CAN_RTR_DATA;
    tx_header.DLC = len;
    tx_header.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_AddTxMessage(&hcan1, &tx_header, (uint8_t *)data, &mailbox) != HAL_OK) {
        return false;
    }

    return true;
}

bool can_if_receive_poll(can_packet_t *pkt)
{
    CAN_RxHeaderTypeDef rx_header = {0};

    if (pkt == 0) {
        return false;
    }

    if (HAL_CAN_GetRxFifoFillLevel(&hcan1, CAN_RX_FIFO0) == 0U) {
        return false;
    }

    if (HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &rx_header, pkt->data) != HAL_OK) {
        return false;
    }

    if ((rx_header.IDE != CAN_ID_STD) || (rx_header.RTR != CAN_RTR_DATA)) {
        return false;
    }

    pkt->id = rx_header.StdId;
    pkt->dlc = rx_header.DLC;
    return true;
}
