#include <Arduino.h>
#include <EEPROM.h>
#include <esp_task_wdt.h>

#include "main.h"
#include "config.h"
#include "user_interface/user_interface.h"
#include "user_interface/drv_rs485_screen/rs485_touch_device.h"
#include "user_interface/drv_terminal/serial_term_device.h"
#include "user_interface/command_dispatcher.h"
#include "modular/encoder.h"
#include "modular/sorter.h"
#include "apps/app_scanner_diag.h"
#include "apps/app_outlet_diag.h"
#include "apps/app_encoder_diag.h"
#include "apps/app_config.h"
#include "apps/app_production.h"
#include "apps/app_about.h"
#include "system/system_manager.h"

// =========================
// 单例实例
// =========================
UserInterface* userInterface = UserInterface::getInstance();
Encoder* encoder = Encoder::getInstance();
DiameterScanner* diameterScanner = DiameterScanner::getInstance();
TraySystem* traySystem = TraySystem::getInstance();
Sorter sorter;

// 业务与诊断处理器
AppProduction appProduction;
AppScannerDiag appScannerDiag;
AppOutletDiag appOutletDiag;
AppEncoderDiag appEncoderDiag;
AppConfigDiameter appConfigDiameter(userInterface, &sorter);
AppConfigPhaseOffset appConfigPhaseOffset(userInterface, &sorter);
AppAbout appAbout;

int normalModeSubmode = 0;
bool hasVersionInfoDisplayed = false;
String systemName = "Feng's AS-L9";

// FreeRTOS 任务句柄
TaskHandle_t hControlTask = nullptr;
TaskHandle_t hUITask = nullptr;

// 任务与辅助函数声明
void vControlTask(void* pvParameters);
void vUITask(void* pvParameters);
void updateActiveApp(uint32_t currentMs, bool btnPressed, int delta);
void setupCommandDispatcher();

// ── 初始化子函数 ────────────────────────────────────────────────────────

/**
 * @brief 配置 HMI 硬件驱动并注入到 UI 抽象层（仅支持 RS485 触摸屏）
 */
void setupHmi() {
    bool anyHmi = false;

    #if ENABLE_HMI_RS485
        Serial.println("[BOOT] HMI: >> Initializing RS485_TOUCH...");
        Rs485TouchDevice* rs485Touch = Rs485TouchDevice::getInstance();
        rs485Touch->initialize();
        userInterface->addTouchDevice(rs485Touch);
        Serial.println("[BOOT] HMI: RS485_TOUCH -> TouchDevice Enabled");
        anyHmi = true;
    #endif

    #if ENABLE_HMI_TERMINAL
        static SerialTermDevice serialTerm;
        serialTerm.initialize();
        userInterface->addTermDevice(&serialTerm);
        Serial.println("[BOOT] HMI: TERMINAL -> TermDevice Enabled");
        anyHmi = true;
    #endif

    if (!anyHmi) {
        Serial.println("[BOOT] HMI: NONE");
    }

    userInterface->initialize();
}

/**
 * @brief 初始化 EEPROM 并加载系统持久化数据
 */
void setupSystemData() {
    EEPROM.begin(512);

    // 1. 处理开机计数
    EEPROM.get(EEPROM_ADDR_BOOT_COUNT, systemBootCount);
    if (systemBootCount == 0xFFFFFFFF) systemBootCount = 0;
    systemBootCount++;
    EEPROM.put(EEPROM_ADDR_BOOT_COUNT, systemBootCount);
    EEPROM.commit();
    Serial.printf("[BOOT] System Boot Count: %lu\n", systemBootCount);

    // 2. 加载相位偏移 (Phase Offset)
    uint8_t phaseOffsetMagic = EEPROM.read(EEPROM_ADDR_PHASE_OFFSET);
    uint8_t phaseOffsetValue = EEPROM.read(EEPROM_ADDR_PHASE_OFFSET + 1);
    uint8_t savedOffset = 0;
    if (phaseOffsetMagic == 0xA5 && phaseOffsetValue < ENCODER_MAX_PHASE) {
        savedOffset = phaseOffsetValue;
    }
    encoder->setPhaseOffset(savedOffset);
    Serial.printf("[BOOT] Phase Offset Loaded: %d\n", savedOffset);

    // 3. 加载托盘历史数据
    traySystem->loadFromEEPROM(EEPROM_ADDR_TRAY_DATA);
}

/**
 * @brief 初始化核心业务模块与诊断处理器
 */
void setupModules() {
    // 初始化分拣执行器与传感器
    encoder->initialize();
    sorter.initialize();
    diameterScanner->initialize();
    
    // 初始化诊断处理器（注入 UI 引用）
    appOutletDiag.initialize(userInterface);
    appEncoderDiag.initialize(userInterface);
    
    // 为诊断处理器绑定出口引用
    for (uint8_t i = 0; i < NUM_OUTLETS; i++) {
        appOutletDiag.setOutlet(i, sorter.getOutlet(i));
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Feng's Sorter System Booting ---");
    
    // 1. 硬件/驱动注入
    setupHmi();
    
    // 1b. 当前业务 Handler 注册到 CommandDispatcher
    setupCommandDispatcher();
    
    // 2. 数据与持久化
    setupSystemData();
    
    // 3. 业务模块初始化
    setupModules();
    
    Serial.println("[BOOT] All systems ready\n");

    // ==========================================
    // 任务创建：将分拣控制核心与 UI 交互核心物理隔离
    // ==========================================
    
    // 1. 创建控制任务 (Core 1, 高优先级, 1000Hz)
    xTaskCreatePinnedToCore(
        vControlTask,   // 任务函数
        "ControlTask",  // 任务名称
        4096,           // 栈大小
        nullptr,        // 参数
        15,             // 优先级
        &hControlTask,  // 句柄
        1               // 绑定到 Core 1
    );

    // 2. 创建 UI 任务 (Core 0, 低优先级, ~33Hz)
    xTaskCreatePinnedToCore(
        vUITask,        // 任务函数
        "UITask",       // 任务名称
        8192,           // 栈大小
        nullptr,        // 参数
        1,              // 优先级
        &hUITask,       // 句柄
        0               // 绑定到 Core 0
    );
}

void vControlTask(void* pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1); // 1ms 循环频率

    Serial.println("[FreeRTOS] ControlTask (Core 1) started.");

    for (;;) {
        // 分拣逻辑消费执行
        if (currentAppType == APP_PRODUCTION || currentAppType == APP_DIAG_OUTLET || currentAppType == APP_DIAG_SCANNER) {
            sorter.run();
        }
        
        // 保持 1ms 的确定性节拍
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void updateActiveApp(uint32_t currentMs, bool btnPressed, int delta) {
    handleAppTypeChange(); // 应用程序状态切换检查

    // 处理当前工作模式逻辑
    if (activeApp) {
        activeApp->update(currentMs, btnPressed);
        
        if (currentAppType == APP_DIAG_OUTLET && delta != 0) {
            appOutletDiag.handleEncoderInput(delta);
        }
    }

    static uint32_t lastSnapshotMs = 0;
    if (currentMs - lastSnapshotMs >= 100) {
        lastSnapshotMs = currentMs;

        // ── 渲染与刷新快照 (中央 Broker 广播机制 -> 推送至 RS485 触摸屏等设备) ──
        DisplaySnapshot snapshot = {};
        snapshot.currentMode = currentAppType;
        
        if (activeApp) {
            activeApp->captureSnapshot(snapshot);
        }
        userInterface->broadcastSnapshot(snapshot);
    }
}

void vUITask(void* pvParameters) {
    Serial.println("[FreeRTOS] UITask (Core 0) started.");

    for (;;) {
        // ── 调度触摸/终端子系统的待处理指令 ──
        userInterface->dispatchPendingCommands();

        // ── 意图驱动输入 ──
        UIIntent intent = userInterface->getNextIntent();
        
        int delta = (intent.action == UIAction::NAVIGATE_RELATIVE) ? intent.value : 0;
        bool btnPressed = (intent.action == UIAction::ACTIVATE);
        
        if (intent.action == UIAction::NAVIGATE_PATH) {
            switchToAppType((AppType)intent.value);
        }
        
        uint32_t currentMs = millis();
        updateActiveApp(currentMs, btnPressed, delta);
        
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ==========================================
// Arduino 框架要求
// ==========================================
void loop() {
    // 原始 loop() 在双核模式下已废弃，任务由 vControlTask 和 vUITask 承载
    // 先注销 loopTask 的看门狗监控，再安全删除释放内存
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

// ── CommandDispatcher Handler 注册 ──────────────────────────────────────────

void setupCommandDispatcher() {
    auto& d = CommandDispatcher::getInstance();

    // ── 页面切换（来自 RS485 / BLE 的 page_sync）
    d.registerHandler("page_sync", [](const RichCommand& cmd) {
        if (cmd.paramI < 0) {
            Serial.printf("[Dispatcher] Unknown page_sync: '%s'\n", cmd.strParam.c_str());
        }
    });

    // ── 出口配置写入（来自 RS485 / BLE 的 set_outlet，更新内存配置）
    d.registerHandler("set_outlet", [](const RichCommand& cmd) {
        if (cmd.index < 0 || cmd.index >= NUM_OUTLETS) return;
        int newMin = (int)(cmd.paramF  + 0.05f);
        int newMax = (int)(cmd.paramF2 + 0.05f);
        uint8_t newMask = (uint8_t)(cmd.paramI & 0x07);
        sorter.setOutletMinDiameter(cmd.index, newMin);
        sorter.setOutletMaxDiameter(cmd.index, newMax);
        sorter.setOutletTargetLength(cmd.index, newMask);
        Serial.printf("[Dispatcher] set_outlet: O%d min=%d max=%d mask=%d\n",
                      cmd.index, newMin, newMax, newMask);
    });

    // ── 持久化保存配置（批量 set_outlet 结束后统一触发一次 EEPROM 写入）
    d.registerHandler("save_config", [](const RichCommand&) {
        sorter.saveConfig();
    });

    // ── 出口诊断（手动控制出口开关，来自 RS485 / BLE 的 diag_outlet）
    d.registerHandler("diag_outlet", [](const RichCommand& cmd) {
        if (cmd.index < 0 || cmd.index >= NUM_OUTLETS) return;
        appOutletDiag.setManualOutletState(cmd.index, cmd.paramB);
        Serial.printf("[Dispatcher] diag_outlet: O%d -> %s\n",
                      cmd.index, cmd.paramB ? "OPEN" : "CLOSE");
    });

    // ── 编码器相位偏移（来自 RS485 / BLE 的 set_offset）
    d.registerHandler("set_offset", [](const RichCommand& cmd) {
        int rawOffset = cmd.paramI;
        int newOffset = rawOffset % ENCODER_MAX_PHASE;
        if (newOffset < 0) newOffset += ENCODER_MAX_PHASE;

        encoder->setPhaseOffset(newOffset);
        EEPROM.write(EEPROM_ADDR_PHASE_OFFSET, 0xA5);
        EEPROM.write(EEPROM_ADDR_PHASE_OFFSET + 1, (uint8_t)newOffset);
        EEPROM.commit();
        Serial.printf("[Dispatcher] set_offset: %d (saved to EEPROM)\n", newOffset);

        // 立即触发一次快照广播推送，确保从机在下一次应答时立即看到更新后的 offset
        DisplaySnapshot snap = {};
        snap.currentMode = currentAppType;
        if (activeApp) activeApp->captureSnapshot(snap);
        userInterface->broadcastSnapshot(snap);
    });

    // ── 清零（Tare，来自 RS485 / BLE）
    d.registerHandler("tare", [](const RichCommand& cmd) {
        Serial.printf("[Dispatcher] tare: params=%d\n", cmd.paramI);
    });

    Serial.println("[Dispatcher] All handlers registered.");
}
