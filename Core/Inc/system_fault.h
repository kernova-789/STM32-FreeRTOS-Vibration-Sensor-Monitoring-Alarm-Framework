#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 进入不可恢复的故障状态：点亮状态绿灯并停止程序。
 * 该函数不会返回。
 */
void system_fault_handle(void);

#ifdef __cplusplus
}
#endif
