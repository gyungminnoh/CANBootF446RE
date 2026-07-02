#include "boot_config.h"
#include "boot_request.h"
#include "can_if.h"
#include "stm32f4xx_hal.h"

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_CAN1_Init(void);
static void app_can_start(void);
static void app_can_poll(void);
static void app_can_send_response(const uint8_t *data, uint8_t len);

CAN_HandleTypeDef hcan1;

int main(void)
{
    SCB->VTOR = APP_START_ADDR;

    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_CAN1_Init();
    app_can_start();

    uint32_t last_blink_ms = HAL_GetTick();

    while (1) {
        app_can_poll();

        if ((HAL_GetTick() - last_blink_ms) >= 250UL) {
            HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
            last_blink_ms = HAL_GetTick();
        }
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 4;
    RCC_OscInitStruct.PLL.PLLN = 180;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 4;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        while (1) {
        }
    }

    if (HAL_PWREx_EnableOverDrive() != HAL_OK) {
        while (1) {
        }
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK) {
        while (1) {
        }
    }
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitStruct.Pin = GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

static void MX_CAN1_Init(void)
{
    hcan1.Instance = CAN1;
    hcan1.Init.Prescaler = 5;
    hcan1.Init.Mode = CAN_MODE_NORMAL;
    hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
    hcan1.Init.TimeSeg1 = CAN_BS1_15TQ;
    hcan1.Init.TimeSeg2 = CAN_BS2_2TQ;
    hcan1.Init.TimeTriggeredMode = DISABLE;
    hcan1.Init.AutoBusOff = ENABLE;
    hcan1.Init.AutoWakeUp = DISABLE;
    hcan1.Init.AutoRetransmission = ENABLE;
    hcan1.Init.ReceiveFifoLocked = DISABLE;
    hcan1.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&hcan1) != HAL_OK) {
        while (1) {
        }
    }
}

static void app_can_start(void)
{
    CAN_FilterTypeDef filter = {0};

    filter.FilterBank = 0;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh = (uint16_t)(CAN_HOST_CMD_ID << 5);
    filter.FilterIdLow = 0x0000U;
    filter.FilterMaskIdHigh = (uint16_t)(0x7FFU << 5);
    filter.FilterMaskIdLow = 0x0000U;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation = ENABLE;
    filter.SlaveStartFilterBank = 14;

    if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK) {
        while (1) {
        }
    }

    if (HAL_CAN_Start(&hcan1) != HAL_OK) {
        while (1) {
        }
    }
}

static void app_can_poll(void)
{
    CAN_RxHeaderTypeDef rx_header = {0};
    uint8_t data[8] = {0};

    if (HAL_CAN_GetRxFifoFillLevel(&hcan1, CAN_RX_FIFO0) == 0U) {
        return;
    }

    if (HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &rx_header, data) != HAL_OK) {
        return;
    }

    if ((rx_header.IDE != CAN_ID_STD) ||
        (rx_header.RTR != CAN_RTR_DATA) ||
        (rx_header.StdId != CAN_HOST_CMD_ID) ||
        (rx_header.DLC == 0U)) {
        return;
    }

    switch (data[0]) {
    case CMD_PING: {
        uint8_t response[8] = {CAN_ACK, CMD_PING, 0xA0U, 0x01U, 0, 0, 0, 0};
        app_can_send_response(response, sizeof(response));
        break;
    }

    case CMD_RESET: {
        boot_request_set();
        uint8_t response[8] = {
            CAN_ACK,
            CMD_RESET,
            boot_request_is_set() ? 1U : 0U,
            0, 0, 0, 0, 0
        };
        app_can_send_response(response, sizeof(response));
        HAL_Delay(20);
        NVIC_SystemReset();
        break;
    }

    default: {
        uint8_t response[8] = {CAN_NACK, data[0], 0x01U, 0, 0, 0, 0, 0};
        app_can_send_response(response, sizeof(response));
        break;
    }
    }
}

static void app_can_send_response(const uint8_t *data, uint8_t len)
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
