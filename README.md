# ESP32-C6 High-Precision Time Synchronization System

This project implements a high-precision time synchronization system for ESP32-C6 using ESP-NOW wireless communication and dedicated hardware timers.

## Features

- **Hardware Timer**: Dedicated GPTimer for 1μs precision timestamps
- **ESP-NOW Protocol**: NTP-like time synchronization over ESP-NOW
- **Master-Slave Architecture**: Automatic role assignment based on MAC address
- **High Precision**: Sub-microsecond accuracy with hardware-level timing
- **Smart Filtering**: Advanced filtering algorithms for stable synchronization
- **Real-time Correction**: Automatic clock correction for slave devices

## Hardware Requirements

- 2x ESP32-C6 development boards
- No additional hardware required (uses built-in WiFi)

## Quick Start

1. **Build and Flash**:
   ```bash
   idf.py build
   idf.py -p COM3 flash  # Master device
   idf.py -p COM7 flash  # Slave device
   ```

2. **Monitor Output**:
   ```bash
   # Monitor master device (COM3)
   idf.py -p COM3 monitor
   
   # Monitor slave device (COM7) - recommended for sync analysis
   idf.py -p COM7 monitor
   ```
   
   **Note**: The slave device shows the most detailed synchronization information including clock corrections and error measurements.

## System Architecture

### Hardware Timer Module (`hw_timer.c/h`)
- 1MHz GPTimer for microsecond precision
- 64-bit counter (virtually no overflow)
- Independent from system ESP timer
- Nanosecond resolution support

### Time Sync Core (`time_sync.c/h`)
- NTP-like algorithm with 4 timestamps
- Smart filtering and error correction
- Automatic peer management
- Comprehensive statistics

### Demo Application (`time_sync_demo.c`)
- Automatic master/slave role assignment
- Real-time synchronization monitoring
- Clean, optimized logging output

## Configuration

Master device MAC: `fc:01:2c:f9:0e:f0` (COM3)
Slave device will sync to this master automatically.

## Performance

- **Precision**: 1 microsecond base resolution
- **Accuracy**: Sub-10μs typical synchronization error
- **Sync Interval**: 10 seconds (configurable)
- **Network Delay**: <1ms over ESP-NOW

## Log Output Guide

The system provides comprehensive logging to monitor synchronization performance:

### Sync Process Logs

**1. Sync Request (Slave → Master)**
```
>>> Sending sync request #1 to master fc:01:2c:f9:0e:f0 <<<
```

**2. Sync Response (Master → Slave)**
```
>>> Sending sync response to slave fc:01:2c:f9:16:28 <<<
```

**3. Measurement Results**
```
Sync measurement: Clock offset=1234us, Network delay=567us
```

### Sync Results Summary

**4. Detailed Sync Results**
```
=== SYNC RESULT #1 ===
Final clock error: 1234us (FAST slave clock)
Network delay: 567us, Quality: 95%
```

**5. Clock Correction (when error > 100μs)**
```
CLOCK CORRECTION APPLIED:
- Error before correction: 1234us
- Correction applied: -1234us
- Total cumulative correction: 0us -> -1234us
Slave clock now synchronized with master
```

**6. No Correction Needed**
```
Clock error within tolerance (<100us), no correction needed
```

### Status Reports (Every 15 seconds)

**7. System Status Summary**
```
=== TIME SYNC STATUS ===
Sync count: 5, Last sync quality: 95%
Current clock error: 12us (EXCELLENT)
Average network delay: 543us
========================
```

### Log Terminology

- **Clock offset**: Raw measured time difference between slave and master
  - Positive = Slave clock is FAST (ahead of master)
  - Negative = Slave clock is SLOW (behind master)

- **Network delay**: Round-trip communication time over ESP-NOW

- **Quality**: Sync quality percentage based on network delay:
  - 95%: <1ms delay (Excellent)
  - 85%: <5ms delay (Good) 
  - 70%: <10ms delay (Fair)
  - 50%: <50ms delay (Poor)

- **Total cumulative correction**: Sum of all clock corrections applied
  - Shows total drift compensation over time

- **Current clock error**: Real-time difference after corrections applied

### Precision Levels

- **PERFECT**: 0μs error (ideal)
- **EXCELLENT**: <10μs error (very high precision)
- **VERY GOOD**: <100μs error (high precision)
- **POOR**: >100μs error (requires attention)

### Timing Schedule

The system operates on the following schedule:

- **Every 10 seconds**: Slave sends sync request to master
- **Every 15 seconds**: Status report is printed
- **Immediate**: Clock correction applied when error > 100μs

**Example Timeline:**
```
0s:  Sync request #1 → Correction applied
10s: Sync request #2 → Small error, no correction
15s: Status report printed
20s: Sync request #3 → Small error, no correction  
30s: Sync request #4 → Status report printed
...
```

## Project Structure

```
├── main/
│   ├── hw_timer.c/h          # Dedicated hardware timer
│   ├── time_sync.c/h         # Core synchronization logic  
│   ├── time_sync_demo.c      # Main application
│   ├── sync_log.h           # Enhanced logging system
│   └── CMakeLists.txt       # Build configuration
├── README.md                # This file
└── sdkconfig                # ESP-IDF configuration
```

This system provides microsecond-precision time synchronization between ESP32-C6 devices without requiring external time sources or additional hardware.