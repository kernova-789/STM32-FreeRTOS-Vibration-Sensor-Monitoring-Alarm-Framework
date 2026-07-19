#pragma once
#include "stdint.h"
#include "stdbool.h"
#include "FreeRTOS.h"
#include "queue.h"

enum indicator_device {
    alarm_led_x,
    alarm_led_y,
    alarm_led_z,

    status_led_red,
    status_led_green,
    status_led_blue,

    buzzer,

    indicator_device_count,
};
struct indicator_msg_t {
    bool use_mask; //true为使用掩码一次性控制所有设备的状态，反之通过id控制单独一个设备

    union {
        uint32_t state_mask;    //每一位控制一个设备，置1为激活

        struct {
            enum indicator_device device_id;
            bool target_state;  //true为激活
            uint32_t duration_ms;  //激活持续时间(ms)，target_state=true时生效，0为永久激活
        } single;
    } data;
};

QueueHandle_t get_indicator_queue(void);
BaseType_t create_indicator_task(void);
