#include "sensor_registry.h"

#include "legacy_can_vibration_sensor.h"

/*
 * 当前产品变体中的两个传感器都使用旧版 16 g CAN 报文。
 * 后续传感器可在自己的表项中指定完全不同的 driver 和 driver_config。
 */
static const legacy_can_vibration_sensor_config_t
    legacy_vibration_profile = {
        .full_scale_centi_g = 1600U,
        .command_prefix = 0xAAU,
};

/*
 * 多传感器配置表。
 *
 * 修改传感器硬件 CAN ID 时，只需调整对应表项的 receive_address 和
 * transmit_address；应用代码和 Flash 配置继续使用 sensor_id。
 * 如果传感器上下行使用不同 CAN ID，可为两个地址填写不同值；
 * 所有接收地址必须保持唯一。
 */
const communication_sensor_config_t product_sensor_table[] = {
    {
        .sensor_id = SENSOR_ID_VIBRATION_PRIMARY,
        .receive_address = UINT32_C(0x001),
        .transmit_address = UINT32_C(0x001),
        .driver = &legacy_can_vibration_sensor_driver,
        .driver_config = &legacy_vibration_profile,
        .driver_context = NULL,
    },
    {
        .sensor_id = SENSOR_ID_VIBRATION_SECONDARY,
        .receive_address = UINT32_C(0x002),
        .transmit_address = UINT32_C(0x002),
        .driver = &legacy_can_vibration_sensor_driver,
        .driver_config = &legacy_vibration_profile,
        .driver_context = NULL,
    },
};

const size_t product_sensor_count =
    sizeof(product_sensor_table) / sizeof(product_sensor_table[0]);
