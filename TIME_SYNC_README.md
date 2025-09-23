# ESP-NOW高精度时间同步

基于ESP-NOW协议实现的高精度时间同步系统，采用类似NTP的算法实现主从设备间的时间戳同步。

## 功能特性

### 🚀 核心功能
- **主从时间同步**：基于NTP四次握手算法的高精度时间同步
- **微秒级精度**：使用`esp_timer_get_time()`提供微秒级时间戳
- **自动角色分配**：根据MAC地址自动分配主机/从机角色
- **智能滤波算法**：内置滑动平均滤波，提升同步稳定性
- **实时统计监控**：提供延迟、偏移量、同步质量等统计信息

### 📡 通信协议
- **传输层**：ESP-NOW低延迟点对点通信
- **同步算法**：NTP-like四时间戳算法
- **数据完整性**：CRC16校验保证数据准确性
- **自动重传**：支持超时重传机制

### ⚙️ 配置参数
- **同步间隔**：可配置1-60秒的同步周期
- **滤波设置**：可选的历史数据滤波功能
- **角色配置**：通过MAC地址自动分配或手动配置

## 系统架构

```
Master Device (时间服务器)          Slave Device (时间客户端)
┌─────────────────────┐            ┌─────────────────────┐
│   时间基准源         │            │   本地时钟          │
│  ┌───────────────┐  │            │  ┌───────────────┐  │
│  │ esp_timer_us  │  │            │  │ esp_timer_us  │  │
│  └───────────────┘  │            │  └───────────────┘  │
│         │            │            │         │           │
│  ┌─────▼─────────┐   │  ESP-NOW   │  ┌─────▼─────────┐  │
│  │ NTP服务器模块  │◄──┼────────────┼──► NTP客户端模块  │  │
│  └───────────────┘   │   协议     │  └───────────────┘  │
│         │            │            │         │           │
│  ┌─────▼─────────┐   │            │  ┌─────▼─────────┐  │
│  │ 时间戳广播     │   │            │  │ 时间偏移计算   │  │
│  └───────────────┘   │            │  └───────────────┘  │
└─────────────────────┘            └─────────────────────┘
```

## NTP同步算法

### 四时间戳交换
```
Client                    Server
  |                         |
  |------ Request --------->| t1: 客户端发送时间
  |      (t1 timestamp)     | t2: 服务器接收时间
  |                         |
  |<----- Response ---------|  t3: 服务器发送时间
  |   (t1,t2,t3,t4)        |  t4: 客户端接收时间
  |                         |
  
计算公式:
offset = ((t2-t1) + (t3-t4)) / 2    # 时间偏移
delay  = (t4-t1) - (t3-t2)          # 往返延迟
```

### 同步质量评估
- **延迟 < 1ms**: 质量95% (优秀)
- **延迟 < 5ms**: 质量85% (良好)  
- **延迟 < 10ms**: 质量70% (一般)
- **延迟 < 50ms**: 质量50% (较差)
- **延迟 ≥ 50ms**: 质量30% (很差)

## API使用说明

### 初始化和配置

```c
#include "time_sync.h"

// 配置时间同步参数
time_sync_config_t config = {
    .role = TIME_SYNC_ROLE_MASTER,  // 或 TIME_SYNC_ROLE_SLAVE
    .sync_interval_ms = 10000,      // 10秒同步间隔
    .timeout_ms = 5000,             // 5秒超时
    .max_retry = 3,                 // 最大重试3次
    .enable_filtering = true,       // 启用滤波
};

// 事件回调函数
void sync_event_handler(time_sync_role_t role, bool success, int64_t offset_us) {
    if (success) {
        ESP_LOGI(TAG, "Sync success: offset=%lld us", offset_us);
    }
}

// 初始化
ESP_ERROR_CHECK(time_sync_init(&config, sync_event_handler));
```

### 设备管理

```c
// 添加对等设备 (从机添加主机)
uint8_t master_mac[] = {0xFC, 0x01, 0x2C, 0xF9, 0x0E, 0xF0};
ESP_ERROR_CHECK(time_sync_add_peer(master_mac));

// 启动同步
ESP_ERROR_CHECK(time_sync_start());
```

### 时间获取

```c
precise_timestamp_t local_time, synced_time;

// 获取本地时间
time_sync_get_local_time(&local_time);

// 获取同步后的时间
time_sync_get_synced_time(&synced_time);

printf("Local: %llu us, Synced: %llu us\\n", 
       local_time.timestamp_us, synced_time.timestamp_us);
```

### 统计信息监控

```c
time_sync_stats_t stats;
time_sync_get_stats(&stats);

printf("Sync count: %u\\n", stats.sync_count);
printf("Current offset: %lld us\\n", stats.current_offset_us);
printf("Last delay: %u us\\n", stats.last_delay_us);
printf("Sync quality: %d%%\\n", stats.sync_quality);
```

## 部署配置

### 1. 确定主从设备

在 `time_sync_demo.c` 中设置主机MAC地址:

```c
// 设置主机MAC地址 (根据实际设备修改)
static uint8_t master_mac[ESP_NOW_ETH_ALEN] = {0xFC, 0x01, 0x2C, 0xF9, 0x0E, 0xF0};
```

### 2. 编译和烧录

```bash
# 配置项目
idf.py menuconfig

# 编译
idf.py build

# 烧录到主机设备 (COM3)
idf.py -p COM3 flash monitor

# 烧录到从机设备 (COM7)
idf.py -p COM7 flash monitor
```

### 3. 配置参数调优

通过 `idf.py menuconfig` -> "Time Synchronization Configuration" 调整:
- 同步间隔 (默认10秒)
- 超时时间 (默认5秒)
- 滤波参数 (默认启用，历史8点)

## 性能指标

### 典型性能
- **同步精度**: ±100微秒 (理想条件)
- **通信延迟**: 1-10毫秒 (ESP-NOW)
- **收敛时间**: 30-60秒 (滤波算法)
- **功耗优化**: 定时同步，低功耗待机

### 影响因素
- **无线环境**: 干扰会影响通信延迟
- **设备距离**: 距离过远增加传输延迟  
- **系统负载**: 高负载影响时间戳精度
- **滤波设置**: 历史点数影响稳定性

## 故障排除

### 常见问题

1. **同步失败**
   - 检查MAC地址配置是否正确
   - 确认设备在ESP-NOW通信范围内
   - 查看日志中的错误信息

2. **延迟过大**
   - 减少无线环境干扰
   - 缩短设备间距离
   - 调整同步间隔

3. **偏移震荡**
   - 启用滤波功能
   - 增加滤波历史点数
   - 检查系统时钟稳定性

### 调试日志

启用详细日志查看同步过程:

```c
esp_log_level_set("time_sync", ESP_LOG_DEBUG);
```

## 扩展功能

### 多从机支持
主机可以同时为多个从机提供时间同步服务，从机需要各自添加主机为对等设备。

### 级联时间同步
支持构建多级时间同步网络，从机也可以作为其他设备的时间源。

### 外部时间源
可以扩展接入GPS、NTP服务器等外部精确时间源。

## 许可证

本项目基于ESP-IDF示例代码开发，遵循相同的开源许可证。