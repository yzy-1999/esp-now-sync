/*
 * Hardware Timer Module Implementation
 * 专用硬件定时器模块实现
 * 
 * 使用ESP32-C6的GPTimer（通用定时器）提供独立的高精度时间戳
 */

#include "hw_timer.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <inttypes.h>

static const char *TAG = "hw_timer";

// 定时器配置
#define HW_TIMER_RESOLUTION_HZ  (1000000)  // 1MHz = 1us resolution
#define HW_TIMER_MAX_COUNT      (UINT64_MAX)

// 全局变量
static gptimer_handle_t g_timer_handle = NULL;
static bool g_timer_initialized = false;
static SemaphoreHandle_t g_timer_mutex = NULL;

// 定时器配置结构
typedef struct {
    uint32_t resolution_hz;     // 定时器分辨率
    uint32_t resolution_ns;     // 单个计数的纳秒数
} hw_timer_config_t;

static hw_timer_config_t g_timer_config = {0};

/**
 * @brief 初始化硬件定时器
 */
esp_err_t hw_timer_init(void)
{
    if (g_timer_initialized) {
        ESP_LOGW(TAG, "Hardware timer already initialized");
        return ESP_OK;
    }
    
    esp_err_t ret = ESP_OK;
    
    // 创建互斥锁
    g_timer_mutex = xSemaphoreCreateMutex();
    if (g_timer_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // 配置GPTimer
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = HW_TIMER_RESOLUTION_HZ,  // 1MHz = 1us精度
        .flags.intr_shared = true,
    };
    
    // 创建定时器
    ret = gptimer_new_timer(&timer_config, &g_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        goto error_cleanup;
    }
    
    // 启用定时器
    ret = gptimer_enable(g_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable timer: %s", esp_err_to_name(ret));
        goto error_cleanup;
    }
    
    // 启动定时器（自由运行模式）
    ret = gptimer_start(g_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        goto error_cleanup;
    }
    
    // 设置配置信息
    g_timer_config.resolution_hz = HW_TIMER_RESOLUTION_HZ;
    g_timer_config.resolution_ns = 1000000000UL / HW_TIMER_RESOLUTION_HZ;  // 1000ns
    
    g_timer_initialized = true;
    
    ESP_LOGI(TAG, "Hardware timer initialized successfully");
    ESP_LOGI(TAG, "Timer resolution: %" PRIu32 " Hz (%" PRIu32 " ns per count)", 
             g_timer_config.resolution_hz, g_timer_config.resolution_ns);
    
    return ESP_OK;
    
error_cleanup:
    if (g_timer_handle) {
        gptimer_disable(g_timer_handle);
        gptimer_del_timer(g_timer_handle);
        g_timer_handle = NULL;
    }
    if (g_timer_mutex) {
        vSemaphoreDelete(g_timer_mutex);
        g_timer_mutex = NULL;
    }
    return ret;
}

/**
 * @brief 去初始化硬件定时器
 */
esp_err_t hw_timer_deinit(void)
{
    if (!g_timer_initialized) {
        return ESP_OK;
    }
    
    esp_err_t ret = ESP_OK;
    
    // 获取互斥锁
    if (xSemaphoreTake(g_timer_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to take mutex for deinit");
    }
    
    // 停止定时器
    if (g_timer_handle) {
        gptimer_stop(g_timer_handle);
        gptimer_disable(g_timer_handle);
        ret = gptimer_del_timer(g_timer_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete timer: %s", esp_err_to_name(ret));
        }
        g_timer_handle = NULL;
    }
    
    g_timer_initialized = false;
    
    // 释放互斥锁并删除
    xSemaphoreGive(g_timer_mutex);
    vSemaphoreDelete(g_timer_mutex);
    g_timer_mutex = NULL;
    
    ESP_LOGI(TAG, "Hardware timer deinitialized");
    return ret;
}

/**
 * @brief 获取硬件定时器时间戳（微秒）
 */
uint64_t hw_timer_get_time_us(void)
{
    if (!g_timer_initialized || !g_timer_handle) {
        ESP_LOGW(TAG, "Timer not initialized");
        return 0;
    }
    
    uint64_t count_value = 0;
    esp_err_t ret = gptimer_get_raw_count(g_timer_handle, &count_value);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get timer count: %s", esp_err_to_name(ret));
        return 0;
    }
    
    // 由于我们设置的分辨率是1MHz，所以计数值直接就是微秒
    return count_value;
}

/**
 * @brief 获取硬件定时器时间戳（纳秒）
 */
uint64_t hw_timer_get_time_ns(void)
{
    if (!g_timer_initialized || !g_timer_handle) {
        ESP_LOGW(TAG, "Timer not initialized");
        return 0;
    }
    
    uint64_t count_value = 0;
    esp_err_t ret = gptimer_get_raw_count(g_timer_handle, &count_value);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get timer count: %s", esp_err_to_name(ret));
        return 0;
    }
    
    // 转换为纳秒：计数值 * 每计数的纳秒数
    return count_value * g_timer_config.resolution_ns;
}

