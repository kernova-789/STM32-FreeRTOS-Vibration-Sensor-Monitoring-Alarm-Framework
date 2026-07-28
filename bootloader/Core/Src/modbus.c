#include "modbus.h"
#include "boot_hardware.h"
#include "boot_ui.h"
#include <stdint.h>
#include "usart.h"
#include "stdbool.h"
#include "string.h"

#define OTA485_FUNC_DATA      0x42
#define OTA485_PACKET_SIZE    263
#define OTA485_DATA_SIZE      256

static uint8_t ota485_tx_buf[8];
static uint8_t app_erased = 0;

static uint8_t OTA485_CheckSum(uint8_t *data, uint16_t len)
{
    uint8_t sum = 0;

    for (uint16_t i = 0; i < len; i++)
    {
        sum += data[i];
    }

    return sum;
}

static void OTA485_EraseApp(void)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error = 0;

    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.PageAddress = APP_ADDRESS;

    /*
     * 只擦除应用程序链接区，保留主程序 settings_store 使用的两个配置页。
     */
    erase_init.NbPages = APP_FLASH_PAGE_COUNT;

    HAL_FLASH_Unlock();

    if (HAL_FLASHEx_Erase(&erase_init, &page_error) != HAL_OK)
    {
        boot_hardware_indicator_set(BOOT_INDICATOR_RED, true);
        boot_ui_show_error();
    }

    HAL_FLASH_Lock();
}
static void RS485_Send(uint8_t *data, uint16_t len) {
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
    HAL_UART_Transmit(&huart2, data, len, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
}

uint8_t OTA485_ProcessPacket(uint8_t *rx_buf, uint16_t rx_len)
{
    static uint32_t nextPack = 0;

    if (rx_len != OTA485_PACKET_SIZE)
    {
        return 0;
    }

    if (rx_buf[1] != OTA485_FUNC_DATA)
    {
        return 0;
    }

    uint8_t check = OTA485_CheckSum(rx_buf, rx_len - 1);

    if (check != rx_buf[rx_len - 1])
    {
        return 0;
    }

    uint32_t num = ((uint32_t)rx_buf[2] << 24);
    num |= ((uint32_t)rx_buf[3] << 16);
    num |= ((uint32_t)rx_buf[4] << 8);
    num |= ((uint32_t)rx_buf[5] << 0);

    if ((num == nextPack) || (num == 0xFFFFFFFF))
    {
        if (app_erased == 0)
        {
            OTA485_EraseApp();
            app_erased = 1;
        }

        if (num != 0xFFFFFFFF)
        {
            writeBinFLASH(APP_ADDRESS + 256 * nextPack,
                          &rx_buf[6],
                          OTA485_DATA_SIZE);
        }

        nextPack++;
    }

    /*
     * 按照旧版代码回复 6 字节：
     * [0] 设备地址
     * [1] 0x41
     * [2~5] nextPack
     */
    ota485_tx_buf[0] = rx_buf[0];
    ota485_tx_buf[1] = rx_buf[1];
    ota485_tx_buf[2] = (nextPack >> 24) & 0xFF;
    ota485_tx_buf[3] = (nextPack >> 16) & 0xFF;
    ota485_tx_buf[4] = (nextPack >> 8) & 0xFF;
    ota485_tx_buf[5] = (nextPack >> 0) & 0xFF;

    RS485_Send(ota485_tx_buf, 6);

    if (num == 0xFFFFFFFF)
    {
        return 1;   // OTA 完成。
    }

    return 0;
}

uint8_t ota485_rx_dma_buf[280];
uint8_t ota485_rx_packet[280];
volatile uint16_t ota485_rx_len = 0;
volatile bool ota485_packet_flag = false;

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART1)
    {
        if (Size <= sizeof(ota485_rx_packet))
        {
            memcpy(ota485_rx_packet, ota485_rx_dma_buf, Size);
            ota485_rx_len = Size;
            ota485_packet_flag = true;
        }

        HAL_UARTEx_ReceiveToIdle_DMA(&huart2,
                                     ota485_rx_dma_buf,
                                     sizeof(ota485_rx_dma_buf));

        if (huart2.hdmarx != NULL)
        {
            __HAL_DMA_DISABLE_IT(huart2.hdmarx, DMA_IT_HT);
        }
    }
}
