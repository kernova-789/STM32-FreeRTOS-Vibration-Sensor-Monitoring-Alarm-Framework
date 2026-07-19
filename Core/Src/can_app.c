#include "can_app.h"
#include "can.h"
#include "FreeRTOS.h"
#include "queue.h"

static QueueHandle_t keyboard_queue = NULL;


HAL_StatusTypeDef send_can_std_frame_8bytes(uint16_t ID, uint8_t *data) {
  // 1. 定义 CAN 发送消息的头部结构体
  CAN_TxHeaderTypeDef tx_header = {0}; // 使用 {0} 初始化所有成员为 0

  // 2. 定义用于接收发送邮箱号的变量 (可选，如果不需要可以传入 NULL)
  uint32_t tx_mailbox;

  // 3. 配置消息头部
  tx_header.StdId =
      (ID & 0x0FFF); // *** 设置标准 ID (11 位) ***
  tx_header.ExtId = 0;                    // 扩展 ID 不使用 (对于标准帧)
  tx_header.RTR = CAN_RTR_DATA;           // 消息类型：数据帧
  tx_header.IDE = CAN_ID_STD;             // ID 类型：标准 ID
  tx_header.DLC = 8;                      // *** 数据长度：8 字节 ***
  tx_header.TransmitGlobalTime = DISABLE; // 不传输全局时间

  // 4. 调用 HAL 库函数发送消息
  //    - hcan1: 已初始化的 CAN 句柄
  //    - &tx_header: 指向配置好的头部结构体的指针
  //    - data: 指向包含 8 字节数据的数组的指针
  //    - &tx_mailbox: 指向存储发送邮箱号的变量的指针 (可选)
  HAL_StatusTypeDef status =
      HAL_CAN_AddTxMessage(&hcan, &tx_header, data, &tx_mailbox);

  // 5. 返回发送状态
  return status;
}
HAL_StatusTypeDef send_can_std_frame_8bytes_to_sensor(uint8_t *data){
  return send_can_std_frame_8bytes(0x001, data);  //0x001为电脑ID,传感器只接受ID为0x001的信息
}

// void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan) {
//   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET); // 亮蓝灯
//   CAN_RxHeaderTypeDef rxHeader;
//   if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rxHeader, FIFO0_recevie_buf) ==
//       HAL_OK) {
//     /* 丢弃标准帧 ID = 0x001 的消息 */
//     if ((rxHeader.IDE == CAN_ID_STD) && (rxHeader.StdId == 0x001)) {
//       HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET); // 灭蓝灯
//       return;
//     }
//     /* 目前除了接收数据没做其他功能，留空用作后续拓展 */
//     if (rxHeader.RTR == CAN_RTR_REMOTE) {
//       /* 如果接收到远程帧，则什么都不做 */
//       HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET); // 灭蓝灯
//       return;
//     }
//     /* 如果接收到数据帧则转发数据 */
//     xQueueSendFromISR(frame_queue, FIFO0_recevie_buf, 0);
//     if (rxHeader.DLC == 0) {
//       /* 如果DLC(有效字节数)为0则直接返回 */
//       HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET); // 灭蓝灯
//       return;
//     }
//   }
//   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET); // 灭蓝灯
// }