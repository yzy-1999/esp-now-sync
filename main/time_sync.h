/*
 * ESP-NOW High Precision Time Synchronization
 * 基于NTP算法的高精度时间同步实现
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_now.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 时间同步角色
typedef enum {
    TIME_SYNC_ROLE_NONE = 0,
    TIME_SYNC_ROLE_MASTER,      // 主机（时间服务器）
    TIME_SYNC_ROLE_SLAVE,       // 从机（时间客户端）
} time_sync_role_t;

// 时间同步包类型
typedef enum {
    TIME_SYNC_REQUEST = 1,      // 时间同步请求
    TIME_SYNC_RESPONSE = 2,     // 时间同步响应
} time_sync_packet_type_t;

// 高精度时间戳结构
typedef struct {
    uint64_t timestamp_us;      // 微秒时间戳
    uint32_t fraction_ns;       // 纳秒小数部分
} __attribute__((packed)) precise_timestamp_t;

// NTP-like时间同步包
typedef struct {
    uint8_t packet_type;        // 包类型
    uint8_t version;            // 协议版本
    uint16_t sequence;          // 序列号
    uint32_t session_id;        // 会话ID
    
    // NTP四个时间戳
    precise_timestamp_t t1;     // 客户端发送时间
    precise_timestamp_t t2;     // 服务器接收时间
    precise_timestamp_t t3;     // 服务器发送时间
    precise_timestamp_t t4;     // 客户端接收时间（在响应包中使用）
    
    // 同步参数
    int64_t offset_us;          // 时间偏移（微秒）
    uint32_t delay_us;          // 往返延迟（微秒）
    uint8_t stratum;            // 时间层级
    uint8_t precision;          // 精度指标
    uint16_t crc16;             // CRC校验
} __attribute__((packed)) time_sync_packet_t;

// 时间同步统计信息
typedef struct {
    uint32_t sync_count;        // 同步次数
    int64_t current_offset_us;  // 当前时间偏移
    uint32_t last_delay_us;     // 最后一次延迟
    uint32_t avg_delay_us;      // 平均延迟
    uint8_t sync_quality;       // 同步质量(0-100)
    uint64_t last_sync_time;    // 最后同步时间
    uint32_t error_count;       // 错误计数
} time_sync_stats_t;

// 时间同步配置
typedef struct {
    time_sync_role_t role;          // 角色
    uint32_t sync_interval_ms;      // 同步间隔(毫秒)
    uint32_t timeout_ms;            // 超时时间
    uint8_t max_retry;              // 最大重试次数
    bool enable_filtering;          // 启用滤波
} time_sync_config_t;

// 时间同步事件回调
typedef void (*time_sync_event_cb_t)(time_sync_role_t role, bool success, int64_t offset_us);

/**
 * @brief 初始化时间同步功能
 * @param config 时间同步配置
 * @param event_cb 事件回调函数
 * @return ESP_OK 成功，其他失败
 */
esp_err_t time_sync_init(const time_sync_config_t *config, time_sync_event_cb_t event_cb);

/**
 * @brief 启动时间同步
 * @return ESP_OK 成功，其他失败
 */
esp_err_t time_sync_start(void);

/**
 * @brief 停止时间同步
 * @return ESP_OK 成功，其他失败
 */
esp_err_t time_sync_stop(void);

/**
 * @brief 去初始化时间同步
 * @return ESP_OK 成功，其他失败
 */
esp_err_t time_sync_deinit(void);

/**
 * @brief 添加对等设备
 * @param mac_addr 设备MAC地址
 * @return ESP_OK 成功，其他失败
 */
esp_err_t time_sync_add_peer(const uint8_t *mac_addr);

/**
 * @brief 删除对等设备
 * @param mac_addr 设备MAC地址
 * @return ESP_OK 成功，其他失败
 */
esp_err_t time_sync_remove_peer(const uint8_t *mac_addr);

/**
 * @brief 获取当前同步的时间戳
 * @param timestamp 输出时间戳
 * @return ESP_OK 成功，其他失败
 */
esp_err_t time_sync_get_synced_time(precise_timestamp_t *timestamp);

/**
 * @brief 获取本地时间戳
 * @param timestamp 输出时间戳
 * @return ESP_OK 成功，其他失败
 */
esp_err_t time_sync_get_local_time(precise_timestamp_t *timestamp);

/**
 * @brief 获取时间同步统计信息
 * @param stats 输出统计信息
 * @return ESP_OK 成功，其他失败
 */
esp_err_t time_sync_get_stats(time_sync_stats_t *stats);

/**
 * @brief 手动触发时间同步
 * @return ESP_OK 成功，其他失败
 */
esp_err_t time_sync_trigger(void);

#ifdef __cplusplus
}
#endif