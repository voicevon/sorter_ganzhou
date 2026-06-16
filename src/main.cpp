#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include <esp_task_wdt.h>

#include "main.h"
#include "config.h"
#include "user_interface/user_interface.h"
#include "user_interface/drv_oled_rotary/oled_knob_device.h"    // Phase4: Façade
#include "user_interface/drv_terminal/serial_term_device.h"    // Phase5
#include "user_interface/drv_rs485_screen/rs485_touch_device.h" // Phase2
#include "user_interface/drv_mcgs/mcgs_touch_device.h"          // Phase3
#include "user_interface/command_dispatcher.h"                  // Phase1
// 旧头文件（期间保留兼容）
#include "user_interface/drv_oled_rotary/oled.h"
#include "user_interface/drv_terminal/terminal.h"
#include "user_interface/common/menu_system.h"
#include "user_interface/drv_rs485_screen/rs485_touch_screen.h"
#include "user_interface/drv_mcgs/mcgs_display.h"
#include "modular/encoder.h"
#include "modular/sorter.h"
#include "apps/app_scanner_diag.h"
#include "apps/app_outlet_diag.h"
#include "apps/app_encoder_diag.h"
#include "apps/app_config.h"
#include "apps/app_hmi_diag.h"
#include "apps/app_production.h"
#include "apps/app_about.h"

#include "user_interface/common/menu_config.h"
#include "system/system_manager.h"

// =========================
// 单例实例
// =========================
UserInterface* userInterface = UserInterface::getInstance();
Encoder* encoder = Encoder::getInstance();
DiameterScanner* diameterScanner = DiameterScanner::getInstance();
TraySystem* traySystem = TraySystem::getInstance();
Sorter sorter;

// 诊断处理器
AppProduction appProduction;
AppScannerDiag appScannerDiag;
AppOutletDiag appOutletDiag;
AppEncoderDiag appEncoderDiag;
AppHmiDiag appHmiDiag(UserInterface::getInstance());
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
void handleReturnToMenu();
void handleMenuMode(const UIIntent& intent);
void updateActiveApp(uint32_t currentMs, bool btnPressed, int delta);
void setupCommandDispatcher(); // Phase7

// ── 初始化子函数 ────────────────────────────────────────────────────────

/**
 * @brief 配置 HMI 硬件驱动并注入到 UI 抽象层
 */
void setupHmi() {
    userInterface->clearAllDisplayDevices();
    userInterface->clearAllInputSources();

    bool anyHmi = false;

    // ── 新范式注册（Phase 2-5）───────────────────────────────────────

    #if ENABLE_HMI_OLED_ROTARY
        // Phase4: Façade —— 旋钮菜单范式
        static OledKnobDevice oledKnob;
        oledKnob.initialize();
        userInterface->addKnobDevice(&oledKnob);
        Serial.println("[BOOT] HMI: OLED_ROTARY -> KnobDevice Enabled");
        anyHmi = true;
    #endif

    #if ENABLE_HMI_RS485
        // Phase2: RS485 —— 触摸直控范式
        Serial.println("[BOOT] HMI: >> Initializing RS485_TOUCH...");
        Rs485TouchDevice* rs485Touch = Rs485TouchDevice::getInstance();
        rs485Touch->initialize();
        userInterface->addTouchDevice(rs485Touch);
        Serial.println("[BOOT] HMI: RS485_TOUCH -> TouchDevice Enabled");
        anyHmi = true;
    #endif

    #if ENABLE_HMI_MCGS
        // Phase3: MCGS —— 触摸直控范式
        static McgsTouchDevice mcgsTouch;
        mcgsTouch.initialize();
        userInterface->addTouchDevice(&mcgsTouch);
        Serial.println("[BOOT] HMI: MCGS -> TouchDevice Enabled");
        anyHmi = true;
    #endif

    #if ENABLE_HMI_TERMINAL
        // Phase5: 终端—— 键盘终端范式（补全输入侧）
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
    Serial.printf("[BOOT] System Boot Count: %u\n", systemBootCount);

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
    
    // 4. 菜单树构建
    setupMenuTree();
    
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
        15,             // 优先级 (最高，确保不被 Modbus 等任务抢占)
        &hControlTask,  // 句柄
        1               // 绑定到 Core 1 (Arduino 默认核)
    );

    // 2. 创建 UI 任务 (Core 0, 低优先级, 30Hz)
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
        // 只有在 Normal 模式或特定的分拣诊断模式下才运行逻辑处理槽
        if (currentAppType == APP_PRODUCTION || currentAppType == APP_DIAG_OUTLET || currentAppType == APP_DIAG_SCANNER) {
            sorter.run();
        }
        
        // 保持 1ms 的确定性节拍
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void handleMenuMode(const UIIntent& intent) {
    bool needsRefresh = false;
    
    if (intent.action == UIAction::NAVIGATE_RELATIVE) {
        menuSystem.handleInput(intent.value, false);
        needsRefresh = true;
    } else if (intent.action == UIAction::ACTIVATE) {
        menuSystem.handleInput(0, true);
        if (menuModeActive) needsRefresh = true;
    } else if (intent.action == UIAction::BACK) {
        handleReturnToMenu();
        needsRefresh = true;
    }
    
    if (needsRefresh) {
        userInterface->renderMenu(menuSystem.getCurrentNode(), menuSystem.getCursorIndex(), menuSystem.getScrollOffset());
    }
}

void updateActiveApp(uint32_t currentMs, bool btnPressed, int delta) {
    handleAppTypeChange(); // 强制应用程序状态检查

    // 处理工作模式逻辑 (主要由 Handler 更新显示)
    if (activeApp) {
        activeApp->update(currentMs, btnPressed);
        
        // 模式特定的附加补丁
        if (currentAppType == APP_DIAG_OUTLET) {
            if (delta != 0) {
                appOutletDiag.handleEncoderInput(delta);
            }
        }
    }

    static uint32_t lastSnapshotMs = 0;
    if (currentMs - lastSnapshotMs >= 100) {
        lastSnapshotMs = currentMs;

        // ── 渲染与刷新快照 (中央 Broker 广播机制) ──
        DisplaySnapshot snapshot = {};
        snapshot.currentMode = currentAppType;
        
        if (activeApp) {
            activeApp->captureSnapshot(snapshot);
        }
        userInterface->refreshAllDevices(snapshot);
    }
}

void vUITask(void* pvParameters) {
    Serial.println("[FreeRTOS] UITask (Core 0) started.");

    for (;;) {
        // ── 调度触摸/终端子系统的待处理指令 ──
        // （必须在 getNextIntent() 之前，预充 _intentQueue）
        userInterface->dispatchPendingCommands();

        // ── 意图驱动输入 ──
        UIIntent intent = userInterface->getNextIntent();
        
        int delta = (intent.action == UIAction::NAVIGATE_RELATIVE) ? intent.value : 0;
        bool btnPressed = (intent.action == UIAction::ACTIVATE);
        
        if (intent.action == UIAction::NAVIGATE_PATH) {
            switchToAppType((AppType)intent.value);
            menuModeActive = false;
        }
        
        uint32_t currentMs = millis();
        
        if (menuModeActive) {
            handleMenuMode(intent);
        } else {
            updateActiveApp(currentMs, btnPressed, delta);
        }
        
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ==========================================
// Arduino 框架要求
// ==========================================
void loop() {
    // 原始 loop() 在双核模式下已废弃，任务由 vControlTask 和 vUITask 承载
    // 为避免看门狗超时重启，需先注销 loopTask 的看门狗监控，再安全删除释放内存
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

void handleReturnToMenu() {
    switchToAppType(APP_PRODUCTION);
    menuModeActive = true;
    hasVersionInfoDisplayed = false;
    
    Serial.println("[MENU] Returned to Main Menu");
    userInterface->clearDisplay();
    userInterface->renderMenu(menuSystem.getCurrentNode(), menuSystem.getCursorIndex(), menuSystem.getScrollOffset());
}

// ── Phase7: CommandDispatcher Handler 注册 ────────────────────────────────────
// 业务 Handler 在此集中注册，驱动层（TouchDevice / TermDevice）不再直接调用业务对象。

void setupCommandDispatcher() {
    auto& d = CommandDispatcher::getInstance();

    // ── 页面切换（来自 RS485 / BLE 的 page_sync，paramI < 0 时由 dispatchPendingCommands 转为 NAVIGATE_PATH）
    // page_sync 中 paramI >= 0 的情况已在 dispatchPendingCommands() 转为 UIIntent(NAVIGATE_PATH)
    // paramI < 0（未知页面）时走此 Handler 处理
    d.registerHandler("page_sync", [](const RichCommand& cmd) {
        if (cmd.paramI < 0) {
            Serial.printf("[Dispatcher] Unknown page_sync: '%s'\n", cmd.strParam.c_str());
        }
    });

    // ── 出口配置写入（来自 RS485 / BLE 的 set_outlet）
    d.registerHandler("set_outlet", [](const RichCommand& cmd) {
        if (cmd.index < 0 || cmd.index >= NUM_OUTLETS) return;
        int newMin = (int)(cmd.paramF  + 0.05f);
        int newMax = (int)(cmd.paramF2 + 0.05f);
        sorter.setOutletMinDiameter(cmd.index, newMin);
        sorter.setOutletMaxDiameter(cmd.index, newMax);
        sorter.saveConfig();
        Serial.printf("[Dispatcher] set_outlet: O%d min=%d max=%d\n",
                      cmd.index, newMin, newMax);
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
        uint8_t newOffset = (uint8_t)(cmd.paramI & 0xFF);
        encoder->setPhaseOffset(newOffset);
        EEPROM.write(EEPROM_ADDR_PHASE_OFFSET, 0xA5);
        EEPROM.write(EEPROM_ADDR_PHASE_OFFSET + 1, newOffset);
        EEPROM.commit();
        Serial.printf("[Dispatcher] set_offset: %d (saved to EEPROM)\n", newOffset);
    });

    // ── MCGS 参数写入（Modbus 寄存器 → EEPROM，来自 McgsTouchDevice）
    d.registerHandler("mcgs_write_param", [](const RichCommand& cmd) {
        uint8_t outletIdx = (uint8_t)cmd.index;
        uint8_t field     = (uint8_t)cmd.paramI;
        uint16_t value    = (uint16_t)cmd.paramF;
        if (outletIdx >= NUM_OUTLETS) return;
        int eepromOffset = EEPROM_ADDR_DIAMETER_DATA + outletIdx * 3;
        switch (field) {
            case 0: EEPROM.write(eepromOffset,     (uint8_t)(value / 100)); break; // MIN
            case 1: EEPROM.write(eepromOffset + 1, (uint8_t)(value / 100)); break; // MAX
            case 2: EEPROM.write(eepromOffset + 2, (uint8_t)(value & 0x07)); break; // LEN_MASK
            case 3: if (outletIdx == 0) EEPROM.write(EEPROM_ADDR_OUTLET0_MODE, (uint8_t)(value & 0x01)); break; // MODE
        }
        Serial.printf("[Dispatcher] mcgs_write_param: O%d field=%d value=%d\n", outletIdx, field, value);
    });

    // ── EEPROM 持久化（来自 McgsTouchDevice 的 CmdConfirm）
    d.registerHandler("eeprom_commit", [](const RichCommand&) {
        EEPROM.commit();
        Serial.println("[Dispatcher] eeprom_commit done.");
    });

    // ── 清零（Tare，来自 RS485 / BLE）
    d.registerHandler("tare", [](const RichCommand& cmd) {
        Serial.printf("[Dispatcher] tare: params=%d\n", cmd.paramI);
        // TODO: 根据 paramI 实现清零逻辑
    });

    Serial.println("[Dispatcher] All handlers registered.");
}

//  测量直径很稳定的版本了
