/*
 * Time Sync Demo Application
 * 时间同步演示应用
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "time_sync.h"

static const char *TAG = "time_sync_demo";

// 配置：根据设备MAC地址自动分配角色
// 这里预设一个主机MAC地址，其他设备自动成为从机
static uint8_t master_mac[ESP_NOW_ETH_ALEN] = {0xFC, 0x01, 0x2C, 0xF9, 0x0E, 0xF0}; // 修改为你的主机MAC

// 获取设备角色
static time_sync_role_t get_device_role(void) {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    
    ESP_LOGI(TAG, "Device MAC: %02x:%02x:%02x:%02x:%02x:%02x", MAC2STR(mac));
    
    // 比较MAC地址
    if (memcmp(mac, master_mac, ESP_NOW_ETH_ALEN) == 0) {
        return TIME_SYNC_ROLE_MASTER;
    } else {
        return TIME_SYNC_ROLE_SLAVE;
    }
}

// WiFi初始化
static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi initialized");
}

// ESP-NOW初始化
static void espnow_init(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_LOGI(TAG, "ESP-NOW initialized");
}

// 时间同步事件回调
static void time_sync_event_handler(time_sync_role_t role, bool success, int64_t offset_us) {
    if (success) {
        ESP_LOGI(TAG, "Time sync successful! Role: %s, Offset: %" PRId64 " us", 
                 role == TIME_SYNC_ROLE_MASTER ? "MASTER" : "SLAVE",
                 offset_us);
    } else {
        ESP_LOGW(TAG, "Time sync failed!");
    }
}

// 统计信息打印任务
static void stats_task(void *pvParameters) {
    time_sync_stats_t stats;
    precise_timestamp_t local_time, synced_time;
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000)); // 每5秒打印一次统计信息
        
        if (time_sync_get_stats(&stats) == ESP_OK) {
            time_sync_get_local_time(&local_time);
            time_sync_get_synced_time(&synced_time);
            
            // 计算时钟误差
            int64_t clock_error_us = (int64_t)synced_time.timestamp_us - (int64_t)local_time.timestamp_us;
            double clock_error_ms = clock_error_us / 1000.0;
            double clock_accuracy_ppm = 0.0;
            if (local_time.timestamp_us > 0) {
                clock_accuracy_ppm = (double)clock_error_us * 1000000.0 / (double)local_time.timestamp_us;
            }
            
            ESP_LOGI(TAG, "=== Time Sync Statistics ===");
            ESP_LOGI(TAG, "Sync Count: %" PRIu32, stats.sync_count);
            ESP_LOGI(TAG, "Current Offset: %" PRId64 " us", stats.current_offset_us);
            ESP_LOGI(TAG, "Last Delay: %" PRIu32 " us", stats.last_delay_us);
            ESP_LOGI(TAG, "Avg Delay: %" PRIu32 " us", stats.avg_delay_us);
            ESP_LOGI(TAG, "Sync Quality: %d%%", stats.sync_quality);
            ESP_LOGI(TAG, "Error Count: %" PRIu32, stats.error_count);
            ESP_LOGI(TAG, "--- Device Clock Analysis ---");
            ESP_LOGI(TAG, "Local Device Time: %" PRIu64 " us", local_time.timestamp_us);
            ESP_LOGI(TAG, "Master Sync Time: %" PRIu64 " us", synced_time.timestamp_us);
            ESP_LOGI(TAG, ">> CLOCK DIFFERENCE: %" PRId64 " us (%.3f ms) <<", clock_error_us, clock_error_ms);
            ESP_LOGI(TAG, "Clock Drift Rate: %.2f ppm", clock_accuracy_ppm);
            
            // 显示时钟差异的含义
            if (clock_error_us > 0) {
                ESP_LOGI(TAG, "Status: Slave clock is BEHIND master by %.3f ms", clock_error_ms);
            } else {
                ESP_LOGI(TAG, "Status: Slave clock is AHEAD of master by %.3f ms", -clock_error_ms);
            }
            
            // 精度等级评估
            const char* precision_level;
            if (llabs(clock_error_us) < 100) {
                precision_level = "EXCELLENT (<100us)";
            } else if (llabs(clock_error_us) < 1000) {
                precision_level = "VERY_GOOD (<1ms)";
            } else if (llabs(clock_error_us) < 10000) {
                precision_level = "GOOD (<10ms)";
            } else {
                precision_level = "NEEDS_IMPROVEMENT (>10ms)";
            }
            ESP_LOGI(TAG, "Precision Level: %s", precision_level);
            ESP_LOGI(TAG, "=============================");
        }
    }
}

void app_main(void) {
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 初始化WiFi和ESP-NOW
    wifi_init();
    espnow_init();
    
    // 确定设备角色
    time_sync_role_t role = get_device_role();
    
    // 配置时间同步
    time_sync_config_t config = {
        .role = role,
        .sync_interval_ms = 10000,  // 10秒同步间隔
        .timeout_ms = 5000,         // 5秒超时
        .max_retry = 3,             // 最大重试3次
        .enable_filtering = true,   // 启用滤波
    };
    
    // 初始化时间同步
    ESP_ERROR_CHECK(time_sync_init(&config, time_sync_event_handler));
    
    // 添加对等设备
    if (role == TIME_SYNC_ROLE_SLAVE) {
        // 从机添加主机作为对等设备
        ESP_ERROR_CHECK(time_sync_add_peer(master_mac));
        ESP_LOGI(TAG, "Added master peer: %02x:%02x:%02x:%02x:%02x:%02x", MAC2STR(master_mac));
    } else {
        // 主机需要根据实际情况添加从机设备
        // 这里可以添加已知的从机MAC地址
        ESP_LOGI(TAG, "Master mode: waiting for slave connections");
    }
    
    // 启动时间同步
    ESP_ERROR_CHECK(time_sync_start());
    
    // 创建统计信息任务
    xTaskCreate(stats_task, "stats", 4096, NULL, 3, NULL);
    
    ESP_LOGI(TAG, "Time sync demo started as %s", 
             role == TIME_SYNC_ROLE_MASTER ? "MASTER" : "SLAVE");
    
    if (role == TIME_SYNC_ROLE_MASTER) {
        ESP_LOGI(TAG, "Master ready. Slaves can now sync to this device.");
        ESP_LOGI(TAG, "Master provides microsecond precision timestamps.");
    } else {
        ESP_LOGI(TAG, "Slave started. Will sync with master: %02x:%02x:%02x:%02x:%02x:%02x", MAC2STR(master_mac));
        ESP_LOGI(TAG, "Note: Update master_mac[] with actual master MAC address for proper sync.");
    }
}