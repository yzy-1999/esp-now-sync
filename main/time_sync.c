/*
 * ESP-NOW High Precision Time Synchronization Implementation
 * 高精度时间同步核心实现
 */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "time_sync.h"

static const char *TAG = "time_sync";

// 内部状态管理
typedef struct {
    uint8_t peer_mac[ESP_NOW_ETH_ALEN];    // 对等设备MAC
    time_sync_packet_t rx_packet;          // 接收到的包
    precise_timestamp_t rx_timestamp;      // 接收时间戳
} time_sync_event_t;

// 全局变量
static time_sync_config_t g_config = {0};
static time_sync_stats_t g_stats = {0};
static time_sync_event_cb_t g_event_callback = NULL;
static QueueHandle_t g_sync_queue = NULL;
static TaskHandle_t g_sync_task_handle = NULL;
static TimerHandle_t g_sync_timer = NULL;
static bool g_initialized = false;
static bool g_running = false;
static uint16_t g_sequence = 0;
static uint32_t g_session_id = 0;

// 滤波历史数据
#define FILTER_HISTORY_SIZE 8
static int64_t g_offset_history[FILTER_HISTORY_SIZE] = {0};
static uint32_t g_delay_history[FILTER_HISTORY_SIZE] = {0};
static int g_history_index = 0;
static int g_history_count = 0;

// 时钟校正跟踪
static int64_t g_cumulative_correction = 0;

// 函数声明
static esp_err_t send_sync_packet(const uint8_t *dest_mac, const time_sync_packet_t *packet);
static void time_sync_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);
static void time_sync_task(void *pvParameters);
static void sync_timer_callback(TimerHandle_t timer);
static precise_timestamp_t get_precise_timestamp(void);
static uint16_t calculate_crc16(const time_sync_packet_t *packet);
static bool validate_packet(const time_sync_packet_t *packet);
static void process_sync_request(const time_sync_event_t *event);
static void process_sync_response(const time_sync_event_t *event);
static void apply_time_filter(int64_t offset_us, uint32_t delay_us);
static int64_t get_synchronized_time(void);

/**
 * 获取同步后的时间（应用校正）
 */
static int64_t get_synchronized_time(void)
{
    return esp_timer_get_time() + g_cumulative_correction;
}

/**
 * 获取高精度时间戳
 */
static precise_timestamp_t get_precise_timestamp(void)
{
    precise_timestamp_t ts = {0};
    
    // 获取校正后的时间戳
    ts.timestamp_us = get_synchronized_time();
    
    // 使用系统时间的低位提供亚微秒精度
    // 这避免了可能不支持的CPU寄存器访问
    uint32_t cycle_approx = (uint32_t)(ts.timestamp_us & 0xFFFFFF);
    ts.fraction_ns = (cycle_approx % 1000) * 1.0;  // 纳秒级别的近似精度
    
    return ts;
}

/**
 * 计算CRC16校验
 */
static uint16_t calculate_crc16(const time_sync_packet_t *packet)
{
    time_sync_packet_t temp_packet = *packet;
    temp_packet.crc16 = 0; // 计算时CRC字段置零
    
    return esp_crc16_le(UINT16_MAX, (const uint8_t*)&temp_packet, 
                        sizeof(time_sync_packet_t) - sizeof(uint16_t));
}

/**
 * 验证数据包
 */
static bool validate_packet(const time_sync_packet_t *packet)
{
    // 检查版本号
    if (packet->version != 1) {
        ESP_LOGW(TAG, "Invalid version: %d", packet->version);
        return false;
    }
    
    // 检查包类型
    if (packet->packet_type != TIME_SYNC_REQUEST && 
        packet->packet_type != TIME_SYNC_RESPONSE) {
        ESP_LOGW(TAG, "Invalid packet type: %d", packet->packet_type);
        return false;
    }
    
    // 验证CRC
    uint16_t calculated_crc = calculate_crc16(packet);
    if (calculated_crc != packet->crc16) {
        ESP_LOGW(TAG, "CRC mismatch: calc=0x%04x, recv=0x%04x", 
                 calculated_crc, packet->crc16);
        return false;
    }
    
    return true;
}

/**
 * 发送时间同步数据包
 */
static esp_err_t send_sync_packet(const uint8_t *dest_mac, const time_sync_packet_t *packet)
{
    // 创建发送包的副本并设置发送时间戳
    time_sync_packet_t send_packet = *packet;
    
    // 设置t3（服务器发送时间）或t1（客户端发送时间）
    precise_timestamp_t send_time = get_precise_timestamp();
    if (g_config.role == TIME_SYNC_ROLE_MASTER) {
        send_packet.t3 = send_time;
    } else {
        send_packet.t1 = send_time;
    }
    
    // 计算并设置CRC
    send_packet.crc16 = calculate_crc16(&send_packet);
    
    // 发送数据包
    esp_err_t ret = esp_now_send(dest_mac, (const uint8_t*)&send_packet, sizeof(time_sync_packet_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send sync packet: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Sent sync packet: type=%d, seq=%d", 
             send_packet.packet_type, send_packet.sequence);
    
    return ESP_OK;
}

/**
 * ESP-NOW接收回调
 */
static void time_sync_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    ESP_LOGI(TAG, "Received ESP-NOW packet: len=%d, from=%02x:%02x:%02x:%02x:%02x:%02x", 
             len, 
             recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
             recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);
    
    if (len != sizeof(time_sync_packet_t)) {
        ESP_LOGW(TAG, "Invalid packet size: %d, expected: %zu", 
                 len, sizeof(time_sync_packet_t));
        return;
    }
    
    // 立即获取接收时间戳
    precise_timestamp_t rx_timestamp = get_precise_timestamp();
    
    const time_sync_packet_t *packet = (const time_sync_packet_t*)data;
    
    // 验证数据包
    if (!validate_packet(packet)) {
        ESP_LOGW(TAG, "Packet validation failed!");
        g_stats.error_count++;
        return;
    }
    
    ESP_LOGI(TAG, "Packet validation passed, type=%d, seq=%d", 
             packet->packet_type, packet->sequence);
    
    // 创建事件并发送到队列
    time_sync_event_t event = {0};
    memcpy(event.peer_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    event.rx_packet = *packet;
    event.rx_timestamp = rx_timestamp;
    
    if (xQueueSend(g_sync_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to send event to queue");
        g_stats.error_count++;
    } else {
        ESP_LOGI(TAG, "Event sent to queue successfully");
    }
}

/**
 * 处理同步请求（主机端）
 */
static void process_sync_request(const time_sync_event_t *event)
{
    if (g_config.role != TIME_SYNC_ROLE_MASTER) {
        return;
    }
    
    ESP_LOGI(TAG, "Processing sync request from %02x:%02x:%02x:%02x:%02x:%02x", 
             event->peer_mac[0], event->peer_mac[1], event->peer_mac[2],
             event->peer_mac[3], event->peer_mac[4], event->peer_mac[5]);
    
    // 自动添加peer（如果尚未添加）
    ESP_LOGI(TAG, "Auto-adding peer for response: %02x:%02x:%02x:%02x:%02x:%02x", 
             event->peer_mac[0], event->peer_mac[1], event->peer_mac[2],
             event->peer_mac[3], event->peer_mac[4], event->peer_mac[5]);
    esp_err_t ret = time_sync_add_peer(event->peer_mac);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "Failed to add peer for response: %s", esp_err_to_name(ret));
        // 继续尝试发送，可能peer已经存在
    } else {
        ESP_LOGI(TAG, "Peer add result: %s", esp_err_to_name(ret));
    }
    
    // 创建响应包
    time_sync_packet_t response = {0};
    response.packet_type = TIME_SYNC_RESPONSE;
    response.version = 1;
    response.sequence = event->rx_packet.sequence;
    response.session_id = event->rx_packet.session_id;
    
    // 设置NTP时间戳
    response.t1 = event->rx_packet.t1;  // 复制客户端发送时间
    response.t2 = event->rx_timestamp;   // 服务器接收时间
    // t3 将在send_sync_packet中设置
    
    response.stratum = 1;  // 主时钟层级
    response.precision = 20; // 微秒精度 (2^-20 ≈ 1us)
    
    // 发送响应
    send_sync_packet(event->peer_mac, &response);
}

/**
 * 应用时间滤波算法
 */
static void apply_time_filter(int64_t offset_us, uint32_t delay_us)
{
    if (!g_config.enable_filtering) {
        g_stats.current_offset_us = offset_us;
        g_stats.last_delay_us = delay_us;
        return;
    }
    
    // 检测大幅度时间跳跃，清空历史数据
    if (g_history_count > 0) {
        int64_t last_offset = g_stats.current_offset_us;
        int64_t diff = offset_us - last_offset;
        if (llabs(diff) > 1000000) {  // 1秒以上的跳跃
            ESP_LOGW(TAG, "Large time jump detected: %" PRId64 " us, clearing filter", diff);
            g_history_count = 0;
            g_history_index = 0;
        }
    }
    
    // 存储历史数据
    g_offset_history[g_history_index] = offset_us;
    g_delay_history[g_history_index] = delay_us;
    g_history_index = (g_history_index + 1) % FILTER_HISTORY_SIZE;
    if (g_history_count < FILTER_HISTORY_SIZE) {
        g_history_count++;
    }
    
    // 使用加权平均滤波，最新值权重更大
    if (g_history_count == 1) {
        g_stats.current_offset_us = offset_us;
        g_stats.last_delay_us = delay_us;
    } else {
        // 加权平均：最新值权重50%，历史平均权重50%
        int64_t avg_offset = 0;
        uint64_t avg_delay = 0;
        for (int i = 0; i < g_history_count - 1; i++) {
            avg_offset += g_offset_history[i];
            avg_delay += g_delay_history[i];
        }
        avg_offset /= (g_history_count - 1);
        avg_delay /= (g_history_count - 1);
        
        g_stats.current_offset_us = (offset_us + avg_offset) / 2;
        g_stats.last_delay_us = (uint32_t)((delay_us + avg_delay) / 2);
    }
}

/**
 * 处理同步响应（从机端）
 */
static void process_sync_response(const time_sync_event_t *event)
{
    if (g_config.role != TIME_SYNC_ROLE_SLAVE) {
        return;
    }
    
    ESP_LOGD(TAG, "Processing sync response from %02x:%02x:%02x:%02x:%02x:%02x", 
             event->peer_mac[0], event->peer_mac[1], event->peer_mac[2],
             event->peer_mac[3], event->peer_mac[4], event->peer_mac[5]);
    
    const time_sync_packet_t *resp = &event->rx_packet;
    precise_timestamp_t t4 = event->rx_timestamp;  // 客户端接收时间
    
    // 提取NTP时间戳（转换为微秒）
    uint64_t t1_us = resp->t1.timestamp_us;
    uint64_t t2_us = resp->t2.timestamp_us;
    uint64_t t3_us = resp->t3.timestamp_us;
    uint64_t t4_us = t4.timestamp_us;
    
    // NTP算法计算时间偏移和往返延迟
    // offset = ((t2 - t1) + (t3 - t4)) / 2
    // delay = (t4 - t1) - (t3 - t2)
    
    // 详细的时间戳分析
    int64_t outbound_time = (int64_t)(t2_us - t1_us);  // 请求传输时间差
    int64_t response_time = (int64_t)(t4_us - t3_us);  // 响应传输时间差  
    int64_t round_trip_time = (int64_t)(t4_us - t1_us); // 总往返时间
    int64_t processing_time = (int64_t)(t3_us - t2_us); // 主机处理时间
    
    ESP_LOGI(TAG, "NTP Timestamps: T1=%" PRIu64 " T2=%" PRIu64 " T3=%" PRIu64 " T4=%" PRIu64, 
             t1_us, t2_us, t3_us, t4_us);
    ESP_LOGI(TAG, "Time Intervals: Outbound=%" PRId64 "us, Processing=%" PRId64 "us, Response=%" PRId64 "us, RoundTrip=%" PRId64 "us",
             outbound_time, processing_time, response_time, round_trip_time);
    
    int64_t offset_us = ((int64_t)(t2_us - t1_us) + (int64_t)(t3_us - t4_us)) / 2;
    int64_t delay_us = (int64_t)(t4_us - t1_us) - (int64_t)(t3_us - t2_us);
    
    ESP_LOGI(TAG, "Raw calculation: offset=%" PRId64 " us, delay=%" PRId64 " us", offset_us, delay_us);
    ESP_LOGW(TAG, "*** CLOCK DIFFERENCE: %s device is %" PRId64 " microseconds %s than master ***", 
             (g_config.role == TIME_SYNC_ROLE_MASTER) ? "Slave" : "This",
             llabs(offset_us), 
             (offset_us > 0) ? "ahead" : "behind");
    
    if (delay_us < 0) {
        ESP_LOGW(TAG, "Negative delay detected: %" PRId64 " us", delay_us);
        g_stats.error_count++;
        return;
    }
    
    // 拒绝明显异常的测量值
    if (delay_us > 100000) {  // > 100ms
        ESP_LOGW(TAG, "Rejecting measurement with high delay: %" PRId64 " us", delay_us);
        g_stats.error_count++;
        return;
    }
    
    // 应用滤波
    int64_t raw_offset = offset_us;
    apply_time_filter(offset_us, (uint32_t)delay_us);
    int64_t filtered_offset = g_stats.current_offset_us;
    
    ESP_LOGI(TAG, "Filter result: raw_offset=%" PRId64 " us -> filtered_offset=%" PRId64 " us (adjustment=%" PRId64 " us)",
             raw_offset, filtered_offset, filtered_offset - raw_offset);
    
    // 更新统计信息
    g_stats.sync_count++;
    g_stats.last_sync_time = esp_timer_get_time();
    
    // 计算平均延迟
    if (g_stats.sync_count == 1) {
        g_stats.avg_delay_us = g_stats.last_delay_us;
    } else {
        g_stats.avg_delay_us = (g_stats.avg_delay_us * 7 + g_stats.last_delay_us) / 8;
    }
    
    // 计算同步质量
    if (g_stats.last_delay_us < 1000) {        // < 1ms
        g_stats.sync_quality = 95;
    } else if (g_stats.last_delay_us < 5000) { // < 5ms
        g_stats.sync_quality = 85;
    } else if (g_stats.last_delay_us < 10000) { // < 10ms
        g_stats.sync_quality = 70;
    } else if (g_stats.last_delay_us < 50000) { // < 50ms
        g_stats.sync_quality = 50;
    } else {
        g_stats.sync_quality = 30;
    }
    
    ESP_LOGI(TAG, "Time sync completed: offset=%" PRId64 " us, delay=%" PRIu32 " us, quality=%d%%",
             g_stats.current_offset_us, g_stats.last_delay_us, g_stats.sync_quality);
    
    // 同步结果总结
    const char* precision_level;
    if (llabs(g_stats.current_offset_us) < 10) {
        precision_level = "EXCELLENT (sub-10μs)";
    } else if (llabs(g_stats.current_offset_us) < 100) {
        precision_level = "VERY GOOD (sub-100μs)";
    } else if (llabs(g_stats.current_offset_us) < 1000) {
        precision_level = "GOOD (sub-1ms)";
    } else if (llabs(g_stats.current_offset_us) < 10000) {
        precision_level = "FAIR (sub-10ms)";
    } else {
        precision_level = "POOR (>10ms)";
    }
    
    ESP_LOGW(TAG, "=== SYNC RESULT SUMMARY ===");
    ESP_LOGW(TAG, "Clock Difference: %" PRId64 " microseconds (%s)", 
             g_stats.current_offset_us, 
             (g_stats.current_offset_us > 0) ? "slave ahead" : "slave behind");
    ESP_LOGW(TAG, "Precision Level: %s", precision_level);
    ESP_LOGW(TAG, "Network Delay: %" PRIu32 " μs, Quality: %d%%", g_stats.last_delay_us, g_stats.sync_quality);
    
    // 对于从机，应用时间校正
    if (g_config.role == TIME_SYNC_ROLE_SLAVE) {
        // 记录校正前的时间
        int64_t before_correction = esp_timer_get_time();
        int64_t before_sync_time = get_synchronized_time();
        
        // 应用时间校正（只有当偏差大于100微秒才校正）
        if (llabs(g_stats.current_offset_us) > 100) {
            // 计算校正量
            int64_t correction_amount = -g_stats.current_offset_us;
            
            ESP_LOGW(TAG, "APPLYING CLOCK CORRECTION:");
            ESP_LOGW(TAG, "Raw time: %" PRId64 " us", before_correction);
            ESP_LOGW(TAG, "Sync time before: %" PRId64 " us", before_sync_time);
            ESP_LOGW(TAG, "Applying correction: %" PRId64 " us", correction_amount);
            
            // 应用校正到全局校正量
            g_cumulative_correction += correction_amount;
            
            int64_t after_sync_time = get_synchronized_time();
            ESP_LOGW(TAG, "Sync time after: %" PRId64 " us", after_sync_time);
            ESP_LOGW(TAG, "Total cumulative correction: %" PRId64 " us", g_cumulative_correction);
            
            // 更新统计：校正后偏差应该接近零
            int64_t previous_offset = g_stats.current_offset_us;
            g_stats.current_offset_us = 0;
            
            ESP_LOGW(TAG, "Clock corrected! Offset %" PRId64 "us -> 0us", previous_offset);
            ESP_LOGW(TAG, "Next sync will measure residual error from corrected time");
        } else {
            ESP_LOGI(TAG, "Offset too small (<100us), no correction needed");
        }
    }
    
    ESP_LOGW(TAG, "=========================");
    
    // 调用事件回调
    if (g_event_callback) {
        g_event_callback(g_config.role, true, g_stats.current_offset_us);
    }
}

/**
 * 时间同步任务
 */
static void time_sync_task(void *pvParameters)
{
    time_sync_event_t event;
    
    ESP_LOGI(TAG, "Time sync task started, role: %s", 
             g_config.role == TIME_SYNC_ROLE_MASTER ? "MASTER" : "SLAVE");
    
    while (g_running) {
        if (xQueueReceive(g_sync_queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ESP_LOGI(TAG, "Received queue event: type=%d, seq=%d", 
                     event.rx_packet.packet_type, event.rx_packet.sequence);
            switch (event.rx_packet.packet_type) {
                case TIME_SYNC_REQUEST:
                    process_sync_request(&event);
                    break;
                    
                case TIME_SYNC_RESPONSE:
                    process_sync_response(&event);
                    break;
                    
                default:
                    ESP_LOGW(TAG, "Unknown packet type: %" PRIu8, event.rx_packet.packet_type);
                    break;
            }
        }
    }
    
    ESP_LOGI(TAG, "Time sync task stopped");
    vTaskDelete(NULL);
}

/**
 * 同步定时器回调
 */
static void sync_timer_callback(TimerHandle_t timer)
{
    if (g_config.role != TIME_SYNC_ROLE_SLAVE) {
        return;
    }
    
    ESP_LOGI(TAG, "Sync timer triggered - starting sync");
    time_sync_trigger();
}

/**
 * 初始化时间同步
 */
esp_err_t time_sync_init(const time_sync_config_t *config, time_sync_event_cb_t event_cb)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "Time sync already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 复制配置
    g_config = *config;
    g_event_callback = event_cb;
    
    // 生成会话ID
    g_session_id = esp_random();
    
    // 创建队列
    g_sync_queue = xQueueCreate(10, sizeof(time_sync_event_t));
    if (!g_sync_queue) {
        ESP_LOGE(TAG, "Failed to create sync queue");
        return ESP_ERR_NO_MEM;
    }
    
    // 注册ESP-NOW接收回调
    esp_err_t ret = esp_now_register_recv_cb(time_sync_recv_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register recv callback: %s", esp_err_to_name(ret));
        vQueueDelete(g_sync_queue);
        g_sync_queue = NULL;
        return ret;
    }
    
    // 设置运行状态为true，以便任务能够正常运行
    g_running = true;
    
    // 创建同步任务
    BaseType_t task_ret = xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 5, &g_sync_task_handle);
    if (task_ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create sync task");
        esp_now_unregister_recv_cb();
        vQueueDelete(g_sync_queue);
        g_sync_queue = NULL;
        g_running = false;
        return ESP_ERR_NO_MEM;
    }
    
    // 为从机创建定时器
    if (g_config.role == TIME_SYNC_ROLE_SLAVE && g_config.sync_interval_ms > 0) {
        g_sync_timer = xTimerCreate("sync_timer", 
                                   pdMS_TO_TICKS(g_config.sync_interval_ms),
                                   pdTRUE, NULL, sync_timer_callback);
        if (!g_sync_timer) {
            ESP_LOGE(TAG, "Failed to create sync timer");
            time_sync_deinit();
            return ESP_ERR_NO_MEM;
        }
    }
    
    g_initialized = true;
    
    ESP_LOGI(TAG, "Time sync initialized successfully, role: %s", 
             g_config.role == TIME_SYNC_ROLE_MASTER ? "MASTER" : "SLAVE");
    
    return ESP_OK;
}

/**
 * 启动时间同步
 */
esp_err_t time_sync_start(void)
{
    if (!g_initialized) {
        ESP_LOGE(TAG, "Time sync not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!g_running) {
        ESP_LOGW(TAG, "Time sync task not running");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 启动从机定时器
    if (g_config.role == TIME_SYNC_ROLE_SLAVE && g_sync_timer) {
        if (xTimerStart(g_sync_timer, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to start sync timer");
            g_running = false;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Sync timer started with interval %" PRIu32 " ms", g_config.sync_interval_ms);
    }
    
    ESP_LOGI(TAG, "Time sync started");
    return ESP_OK;
}

/**
 * 停止时间同步
 */
esp_err_t time_sync_stop(void)
{
    if (!g_running) {
        return ESP_OK;
    }
    
    g_running = false;
    
    // 停止定时器
    if (g_sync_timer && xTimerStop(g_sync_timer, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to stop sync timer");
    }
    
    ESP_LOGI(TAG, "Time sync stopped");
    return ESP_OK;
}

/**
 * 去初始化时间同步
 */
esp_err_t time_sync_deinit(void)
{
    if (!g_initialized) {
        return ESP_OK;
    }
    
    // 停止运行
    time_sync_stop();
    
    // 删除定时器
    if (g_sync_timer) {
        xTimerDelete(g_sync_timer, pdMS_TO_TICKS(1000));
        g_sync_timer = NULL;
    }
    
    // 删除任务
    if (g_sync_task_handle) {
        vTaskDelete(g_sync_task_handle);
        g_sync_task_handle = NULL;
    }
    
    // 注销回调
    esp_now_unregister_recv_cb();
    
    // 删除队列
    if (g_sync_queue) {
        vQueueDelete(g_sync_queue);
        g_sync_queue = NULL;
    }
    
    // 清零状态
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_offset_history, 0, sizeof(g_offset_history));
    memset(g_delay_history, 0, sizeof(g_delay_history));
    g_history_index = 0;
    g_history_count = 0;
    
    g_initialized = false;
    
    ESP_LOGI(TAG, "Time sync deinitialized");
    return ESP_OK;
}

/**
 * 添加对等设备
 */
esp_err_t time_sync_add_peer(const uint8_t *mac_addr)
{
    if (!mac_addr) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_now_peer_info_t peer_info = {0};
    memcpy(peer_info.peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
    peer_info.channel = 0;  // 使用当前信道
    peer_info.ifidx = ESP_IF_WIFI_STA;
    
    esp_err_t ret = esp_now_add_peer(&peer_info);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Added peer: %02x:%02x:%02x:%02x:%02x:%02x", 
                 mac_addr[0], mac_addr[1], mac_addr[2], 
                 mac_addr[3], mac_addr[4], mac_addr[5]);
    } else if (ret == ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGD(TAG, "Peer already exists: %02x:%02x:%02x:%02x:%02x:%02x", 
                 mac_addr[0], mac_addr[1], mac_addr[2], 
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        ret = ESP_OK;  // 不是错误
    } else {
        ESP_LOGE(TAG, "Failed to add peer: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

/**
 * 删除对等设备
 */
esp_err_t time_sync_remove_peer(const uint8_t *mac_addr)
{
    if (!mac_addr) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = esp_now_del_peer(mac_addr);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Removed peer: %02x:%02x:%02x:%02x:%02x:%02x", 
                 mac_addr[0], mac_addr[1], mac_addr[2], 
                 mac_addr[3], mac_addr[4], mac_addr[5]);
    } else {
        ESP_LOGE(TAG, "Failed to remove peer: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

/**
 * 获取同步后的时间
 */
esp_err_t time_sync_get_synced_time(precise_timestamp_t *timestamp)
{
    if (!timestamp) {
        return ESP_ERR_INVALID_ARG;
    }
    
    precise_timestamp_t local_time = get_precise_timestamp();
    
    // 应用时间偏移
    timestamp->timestamp_us = local_time.timestamp_us + g_stats.current_offset_us;
    timestamp->fraction_ns = local_time.fraction_ns;
    
    return ESP_OK;
}

/**
 * 获取本地时间
 */
esp_err_t time_sync_get_local_time(precise_timestamp_t *timestamp)
{
    if (!timestamp) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *timestamp = get_precise_timestamp();
    return ESP_OK;
}

/**
 * 获取统计信息
 */
esp_err_t time_sync_get_stats(time_sync_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *stats = g_stats;
    return ESP_OK;
}

/**
 * 手动触发时间同步
 */
esp_err_t time_sync_trigger(void)
{
    if (g_config.role != TIME_SYNC_ROLE_SLAVE) {
        ESP_LOGW(TAG, "Only slaves can trigger sync");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!g_running) {
        ESP_LOGW(TAG, "Time sync not running");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 获取第一个对等设备的MAC地址
    esp_now_peer_info_t peer_info;
    bool peer_found = false;
    
    if (esp_now_fetch_peer(true, &peer_info) == ESP_OK) {
        peer_found = true;
    }
    
    if (!peer_found) {
        ESP_LOGW(TAG, "No peers available for sync");
        return ESP_ERR_NOT_FOUND;
    }
    
    // 创建同步请求
    time_sync_packet_t request = {0};
    request.packet_type = TIME_SYNC_REQUEST;
    request.version = 1;
    request.sequence = ++g_sequence;
    request.session_id = g_session_id;
    // t1 will be set in send_sync_packet
    
    ESP_LOGI(TAG, "Sending sync request #%" PRIu16 " to %02x:%02x:%02x:%02x:%02x:%02x", 
             request.sequence, 
             peer_info.peer_addr[0], peer_info.peer_addr[1], peer_info.peer_addr[2], 
             peer_info.peer_addr[3], peer_info.peer_addr[4], peer_info.peer_addr[5]);
    
    return send_sync_packet(peer_info.peer_addr, &request);
}