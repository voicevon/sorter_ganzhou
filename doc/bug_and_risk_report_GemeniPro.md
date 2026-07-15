# `esp32_sorter` 项目 Bug 与风险 (Risks) 分析报告

通过对项目源码（特别是 `main.cpp`、`SystemManager`、`sorter.cpp`、`encoder.cpp` 以及驱动层）的深度检索与分析，发现了以下几个严重的架构 Bug 和稳定性风险，它们主要集中在**实时操作系统的任务调度、中断机制与数据并发安全**上。

## 🐞 1. 严重的任务优先级倒置 (Priority Inversion)
*   **出处**：`main.cpp` 和 `utils/ModbusMaster.cpp`
*   **Bug 描述**：
    *   核心分拣控制任务 `vControlTask` 被绑定在 Core 1，要求以极高的实时性运行（1ms 的 `vTaskDelayUntil` 频率），但其优先级被设置为 **`10`**。
    *   `ModbusMaster` 内部负责处理 HMI 串口数据的 `modbusTask` 同样被绑定在 Core 1，但其优先级却被设置为了 **`15`**。
*   **致命后果**：FreeRTOS 是抢占式调度。这意味着只要串口有数据进来，优先级高达 15 的 Modbus 任务就会立刻抢占 `vControlTask`。如果 Modbus 的 CRC 校验、内存拷贝或响应延迟超过 1 毫秒，就会直接破坏分拣任务的硬实时性，导致打料电磁铁动作延迟、甚至打飞（漏拣）。

## 🐞 2. 在 1ms 核心实时任务中使用阻塞型 I/O
*   **出处**：`modular/sorter.cpp` -> `DiameterScanner::getDiameterAndStop()`
*   **Bug 描述**：`Sorter::run()` 运行在 1ms 的核心控制任务中。当触发锁存阶段 (`flagDataLatch == true`) 时，会调用 `scanner->getDiameterAndStop()`，而这个函数内部赫然写着一句 `Serial.printf("[SCANNER_DEBUG]...")`。
*   **致命后果**：`Serial.printf` 是线程阻塞操作。当 UART 发送缓冲区(Tx Buffer)被填满时，`printf` 所在任务会被挂起，直到硬件把字符发完。在 1ms 周期的实时任务中打印长字符串，必定会导致任务超时崩溃（Miss Deadline），从而破坏分拣时序。

## 🐞 3. 中断上下文的数据竞争 (Data Race) 与 volatile 缺失
*   **出处**：`modular/encoder.h`
*   **Bug 描述**：`Encoder` 类的几个关键核心计数值（如 `long rawEncoderCount;`、`long zeroCrossCount;`、`forcedZeroCount`）都是在极高频的外部硬件中断 (`handleAPhaseInterrupt` 等) 内部修改的。
*   **致命后果**：这些被中断修改的变量**没有加上 `volatile` 关键字声明**，也没有使用 `std::atomic`。C++ 编译器极有可能会为了优化，将这些变量的值缓存在 CPU 寄存器中。结果就是 `vControlTask` 中的 `encoder->getRawCount()` 读到的永远是旧数据（脏读），导致机器判断失误。

## ⚠️ 4. 运行态下的 EEPROM Flash 擦写风险 (高危 Risk)
*   **出处**：`McgsDisplay.cpp`, `rs485_touch_screen.cpp`, `app_config.cpp`
*   **风险描述**：系统允许用户在 HMI (触摸屏) 端修改参数（例如：直径范围、编码器零位偏移）。一旦用户在屏幕上按下保存，代码会立即调用 `EEPROM.commit()`。
*   **后果**：ESP32 的 `EEPROM.commit()` 会触发内部 Flash 的擦写。在 Flash 擦写期间，ESP32 **会禁用相关的硬件中断或导致 Core Stall（核心停滞）**。如果流水线正在高速运转时用户按下了保存，机器会在这几毫秒到几十毫秒内完全“失聪”，漏掉大量编码器脉冲，导致后面所有的物品分拣位置发生致命漂移。
*   **修复建议**：绝对禁止在流水线运行期间调用 `commit()`。应该使用脏数据标记 (Dirty Flag)，等检测到传送带速度为 0（电机停机）时，再异步执行 Flash 写入。

## ⚠️ 5. 跨上下文的数组撕裂风险 (Risk)
*   **出处**：`modular/diameter_scanner.cpp`
*   **风险描述**：`sample(phase)` 运行在中断 (ISR) 中，不断向 `sensorBuffers` 数组写入数据；而 `getDiameterAndStop()` 运行在常规任务中。虽然代码用 `std::atomic<bool> isScanning` 和 `stop()` 做了防护，但 `stop()` 执行的瞬间与中断触发在多核 ESP32 环境下仍有微小概率发生交错，可能导致 `sensorBuffers` 末尾的数据和 `sampleCount` 数量不匹配。

### 总结修复优先级
1.  **马上降级 `ModbusMaster` 的任务优先级** (建议设为 1 ~ 3)，绝不能高于分拣主任务。
2.  **删除 `DiameterScanner::getDiameterAndStop()` 里面的 `Serial.printf`** 或改用异步消息队列打印。
3.  对 `encoder.h` 中的 `rawEncoderCount` 加上 **`volatile`** 或 **`std::atomic<long>`** 修饰符。
4.  优化参数持久化逻辑，避开系统运行期间的 `EEPROM.commit()` 动作。
