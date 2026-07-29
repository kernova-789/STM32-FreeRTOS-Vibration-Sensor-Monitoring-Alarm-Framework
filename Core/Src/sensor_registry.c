#include "sensor_registry.h"

#include "legacy_can_vibration_sensor.h"

/* 当前产品使用一个旧版 16 g CAN 振动传感器。 */
static const legacy_can_vibration_sensor_config_t
    legacy_vibration_profile = {
        .full_scale_centi_g = 1600U,
        .command_prefix = 0x01U,
};

/* 旧协议使用方向相关的 CAN ID：0x001 发命令，0x002 返回振动数据。 */
const communication_sensor_config_t product_sensor_table[] = {
    {
        .sensor_id = SENSOR_ID_VIBRATION_PRIMARY,
        .receive_address = UINT32_C(0x188),
        .transmit_address = UINT32_C(0x001),
        .driver = &legacy_can_vibration_sensor_driver,
        .driver_config = &legacy_vibration_profile,
        .driver_context = NULL,
    },
};

const size_t product_sensor_count =
    sizeof(product_sensor_table) / sizeof(product_sensor_table[0]);
