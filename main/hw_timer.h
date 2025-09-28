/*
 * Hardware Timer Module for Time Synchronization
 * 用于时间同步的专用硬件定时器模块
 * 
 * 使用ESP32-C6的GPTimer提供高精度时间戳
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化硬件定时器
 * @return ESP_OK 成功，其他失败
 */
esp_err_t hw_timer_init(void);

/**
 * @brief 去初始化硬件定时器
 * @return ESP_OK 成功，其他失败
 */
esp_err_t hw_timer_deinit(void);

/**
 * @brief 获取硬件定时器时间戳（微秒）
 * @return 64位微秒时间戳
 */
uint64_t hw_timer_get_time_us(void);

/**
 * @brief 获取硬件定时器时间戳（纳秒）
 * @return 64位纳秒时间戳
 */
uint64_t hw_timer_get_time_ns(void);


#ifdef __cplusplus
}
#endif
