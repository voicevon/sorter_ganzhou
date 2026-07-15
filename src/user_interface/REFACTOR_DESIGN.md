# UserInterface 重构设计文档（方案 C）

**适用项目**：`esp32_sorter`  
**文档路径**：`src/user_interface/REFACTOR_DESIGN.md`  
**状态**：设计稿（待审阅）

---

## 一、核心认知：两个正交维度

现有代码将**UI 交互范式**与**通讯传输层**混在同一个驱动类里，是根本性的架构问题。

```
现有：一个类 = 范式 + 传输 混合在一起

  drv_oled_rotary/   → 旋钮范式  × I2C 传输
  drv_rs485_screen/  → 触摸范式  × RS485 传输
  drv_mcgs/          → 触摸范式  × RS485/Modbus 传输
  drv_terminal/      → 终端范式  × USB Serial 传输 (仅输出，无输入)
```

重构后：**两个维度正交分离**

```
范式层（Paradigm）       传输层（Transport）
─────────────────       ─────────────────────
KnobUI  (旋钮菜单)  ←→  Local: I2C OLED + GPIO 编码器
TouchUI (触摸直控)  ←→  Local: SPI LCD (LVGL)
                    ←→  Remote: RS485 / Modbus
                    ←→  Remote: BLE (手机/平板)
TerminalUI (终端)   ←→  Local: USB Serial
                    ←→  Remote: BLE UART (nRF UART Service)
```

---

## 二、三种交互范式的本质差异

### 2.1 KnobUI（旋钮菜单范式）

**特征：**
- 操作空间是一棵**菜单树**，用户在树上游走
- 输入语义：`NAVIGATE_RELATIVE(delta)` / `ACTIVATE` / `BACK` / `CANCEL`
- 输出语义：渲染当前 `MenuNode`（title + items + cursor）
- 状态快照可选：可以在叶节点（App）页面显示数据

**当前实现：**
- 输入：`RotaryInputSource`（基于中断，产生 UIIntent）
- 输出：`OLED`（渲染 MenuNode + DisplaySnapshot）

**特点：** 输入输出是**分离的硬件**（编码器 ≠ OLED 屏），需要分别注册。

---

### 2.2 TouchUI（触摸直控范式）

**特征：**
- 操作空间是**页面（Page）**的集合，用户可以直接跳转
- 对端有自己的 UI 渲染引擎（LVGL / HTML / App）
- 双向协议：主控推数据 ↔ 对端推事件
- 事件语义**远超 UIIntent 能表达的范围**（"把出口2的最小直径设为3.5"）
- 输入输出绑定在**同一个通讯通道**上

**当前实现（有架构妥协）：**
- `Rs485TouchScreen`：同时是 `Display` + `InputSource`，但 cpp 里直接调用业务对象
- `McgsDisplay`：类似结构

**问题：** 驱动层直接 `#include sorter.h / encoder.h / app_outlet_diag.h`，驱动与业务强耦合。

---

### 2.3 TerminalUI（键盘终端范式）

**特征：**
- 输出：ASCII 文本（不需要图形渲染）
- 输入：命令行字符串（`NAV:+1`, `ACTIVATE`, `set_offset:12` 等）
- 适合**调试、远程监控、自动化测试**场景
- 可运行在任何字节流传输上（USB Serial / BLE UART / TCP）

**当前实现（残缺）：**
- `Terminal`：只实现了输出（`Display`），**完全没有输入**
- 没有对应的 `TerminalInputSource`

---

## 三、重构目标

1. **范式层与传输层解耦**：驱动类只负责"数据怎么传"，不再直接调用业务对象
2. **引入富指令总线（`RichCommand`）**：承载触摸范式的复杂操作，替代驱动直接调业务
3. **补全 TerminalUI 输入侧**：让串口终端既能输出也能接收命令
4. **BLE 以正确的范式接入**：`BleTouch`（手机 App）/ `BleTerm`（调试 UART）均可插拔
5. **main.cpp 注册方式保持简洁**，不增加理解负担

---

## 四、新架构总览

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         UserInterface（顶层调度器）                        │
│                                                                         │
│  ┌────────────────┐  ┌───────────────────────────┐  ┌───────────────┐  │
│  │  KnobSubsystem │  │    TouchSubsystem          │  │ TermSubsystem │  │
│  │  旋钮菜单子系统  │  │    触摸直控子系统            │  │ 终端子系统     │  │
│  └────────┬───────┘  └────────────┬──────────────┘  └──────┬────────┘  │
│           │                       │                          │           │
│           │ UIIntent              │ RichCommand              │ TextCmd   │
│           │ (导航意图)             │ (富指令)                  │ (文本命令) │
└───────────┼───────────────────────┼──────────────────────────┼───────────┘
            │                       │                          │
     ┌──────┴──────┐         ┌──────┴──────┐          ┌───────┴───────┐
     │ IKnobDevice │         │ITouchDevice │          │ ITermDevice   │
     └──────┬──────┘         └──────┬──────┘          └───────┬───────┘
            │                       │                          │
  ┌─────────┼──────┐    ┌───────────┼───────────┐    ┌────────┼────────┐
  │         │      │    │           │           │    │        │        │
 OLED  Rotary   (模拟) Rs485   McgsDisplay  BleTouch Serial  BleTerm  (TCP)
(I2C)  (GPIO)       (RS485)  (Modbus RTU)  (BLE)           (BLE UART)
```

---

## 五、新接口设计

### 5.1 IKnobDevice（旋钮菜单设备接口）

```cpp
// src/user_interface/paradigm/i_knob_device.h

class IKnobDevice {
public:
    virtual ~IKnobDevice() = default;

    // 设备心跳（轮询/中断收割）
    virtual void tick() = 0;

    // 是否有待处理的意图
    virtual bool hasIntent() = 0;

    // 消费意图（仅产生导航类意图）
    virtual UIIntent pollIntent() = 0;

    // 渲染菜单状态到本地显示器
    virtual void renderMenu(MenuNode* node, int cursor, int scroll) = 0;

    // 渲染 App 数据快照（可选实现，不需要可留空）
    virtual void refreshSnapshot(const DisplaySnapshot& snap) {}

    virtual bool isAvailable() const = 0;
    virtual String getName() const = 0;
};
```

**实现类（现有 → 改造）：**
- `OledKnob`：合并现有 `OLED` + `RotaryInputSource`，**两个硬件合为一个设备对象**

---

### 5.2 ITouchDevice（触摸直控设备接口）

```cpp
// src/user_interface/paradigm/i_touch_device.h

// 富指令结构体（替代驱动直接调业务）
struct RichCommand {
    String   cmd;       // "set_outlet" / "set_offset" / "diag_outlet" / "page_sync" 等
    int      index;     // 对象索引（出口号、编码器通道等）
    float    paramF;    // 浮点参数（min/max 直径）
    int      paramI;    // 整型参数（偏移量、状态值）
    bool     paramB;    // 布尔参数（开关、使能）
    String   strParam;  // 字符串参数（页面名称等）
};

class ITouchDevice {
public:
    virtual ~ITouchDevice() = default;

    // 设备心跳
    virtual void tick() = 0;

    // 推送状态快照（主控 → 对端显示）
    virtual void pushSnapshot(const DisplaySnapshot& snapshot) = 0;

    // 是否有待处理的富指令
    virtual bool hasCommand() = 0;

    // 消费富指令（对端 → 主控操作）
    virtual RichCommand pollCommand() = 0;

    virtual bool isAvailable() const = 0;
    virtual String getName() const = 0;
};
```

**实现类（现有 → 改造）：**
- `Rs485TouchDevice`：改造自 `Rs485TouchScreen`，`processLine()` 改为产生 `RichCommand` 而非直接调业务
- `McgsTouchDevice`：改造自 `McgsDisplay`
- `BleTouchDevice`：新增，手机 App 通过 BLE 连接

---

### 5.3 ITermDevice（终端设备接口）

```cpp
// src/user_interface/paradigm/i_term_device.h

class ITermDevice {
public:
    virtual ~ITermDevice() = default;

    // 设备心跳（轮询字符流）
    virtual void tick() = 0;

    // 推送文本（主控 → 终端显示）
    virtual void print(const String& text) = 0;
    virtual void println(const String& text) = 0;

    // 推送结构化快照（转换为文本输出）
    virtual void pushSnapshot(const DisplaySnapshot& snapshot) = 0;

    // 是否有待处理的文本命令
    virtual bool hasCommand() = 0;

    // 消费文本命令行（已去除换行符）
    virtual String pollCommand() = 0;

    virtual bool isAvailable() const = 0;
    virtual String getName() const = 0;
};
```

**实现类：**
- `SerialTermDevice`：改造自 `Terminal`，补全输入侧（读 `Serial.read()`，解析命令行）
- `BleTermDevice`：新增，使用 nRF UART Service，走 BLE 字节流

---

## 六、富指令调度器（CommandDispatcher）

这是解耦触摸范式驱动与业务层的关键：

```cpp
// src/user_interface/command_dispatcher.h

class CommandDispatcher {
public:
    // 注册命令处理器（业务层在 setup() 中注册）
    using Handler = std::function<void(const RichCommand&)>;
    void registerHandler(const String& cmd, Handler handler);

    // 分发一条富指令（由 TouchSubsystem 调用）
    void dispatch(const RichCommand& cmd);

    // 将文本命令解析为 RichCommand 再分发（由 TermSubsystem 调用）
    void dispatchText(const String& line);

private:
    std::map<String, Handler> _handlers;
    RichCommand parseText(const String& line);
};
```

**main.cpp 注册示例（业务层注册自己的命令处理器）：**

```cpp
void setupCommandDispatcher() {
    auto& disp = CommandDispatcher::getInstance();

    disp.registerHandler("set_outlet", [](const RichCommand& cmd) {
        sorter.setOutletMinDiameter(cmd.index, (int)(cmd.paramF + 0.05f));
        sorter.setOutletMaxDiameter(cmd.index, (int)(cmd.paramF2 + 0.05f));
        sorter.saveConfig();
    });

    disp.registerHandler("set_offset", [](const RichCommand& cmd) {
        encoder->setPhaseOffset(cmd.paramI);
        // 持久化到 EEPROM...
    });

    disp.registerHandler("diag_outlet", [](const RichCommand& cmd) {
        appOutletDiag.setManualOutletState(cmd.index, cmd.paramB);
    });

    disp.registerHandler("page_sync", [](const RichCommand& cmd) {
        // 将页面名称映射为 AppType，触发模式切换
        switchToAppType(pageNameToAppType(cmd.strParam));
    });
}
```

`Rs485TouchDevice::processLine()` 改造后只产生 `RichCommand`，完全不 `#include` 业务头文件。

---

## 七、新 UserInterface 内部结构

```cpp
class UserInterface {
    // ── 旋钮菜单子系统 ──
    IKnobDevice* knobDevices[MAX_KNOB_DEVICES];
    int knobDeviceCount;

    // ── 触摸直控子系统 ──
    ITouchDevice* touchDevices[MAX_TOUCH_DEVICES];
    int touchDeviceCount;

    // ── 终端子系统 ──
    ITermDevice* termDevices[MAX_TERM_DEVICES];
    int termDeviceCount;

    // ── 富指令调度器（共享） ──
    CommandDispatcher* dispatcher;

public:
    // 注册设备
    void addKnobDevice(IKnobDevice* dev);
    void addTouchDevice(ITouchDevice* dev);
    void addTermDevice(ITermDevice* dev);

    // 获取下一个意图（仅来自旋钮子系统）
    UIIntent getNextIntent();

    // 广播快照到所有子系统
    void broadcastSnapshot(const DisplaySnapshot& snapshot);

    // 调度所有子系统的待处理指令（在主循环中调用）
    void dispatchPendingCommands();

    // 菜单渲染（仅广播到旋钮子系统）
    void renderMenu(MenuNode* node, int cursor, int scroll);
};
```

---

## 八、BLE 驱动设计

BLE 在本架构下是**纯传输层**，可以承载两种范式：

### 8.1 BleTouchDevice（手机 App 范式）

```
src/user_interface/drv_ble/
├── ble_manager.h/cpp       BLE Stack 单例（配对、GATT Server）
├── ble_touch_device.h/cpp  实现 ITouchDevice
│                           pushSnapshot() → 二进制 Notify
│                           tick()        → 解析 JSON Write → RichCommand
└── ble_term_device.h/cpp   实现 ITermDevice（可选，用于调试）
                            使用 nRF UART Service（Nordic UUID）
```

**GATT 服务（与 v2.0 一致）：**

```
STATUS  (Read + Notify)  → 推送二进制状态包（同 v2.0 协议）
COMMAND (Write)          → 接收 JSON，解析为 RichCommand
```

**JSON 指令格式（与 Rs485TouchScreen 完全对齐）：**
```json
{"page": "diag_encoder"}
{"events": [{"cmd": "set_outlet", "index": 2, "min": 3.5, "max": 6.0, "mask": 3}]}
{"events": [{"cmd": "set_offset", "state": 12}]}
```

### 8.2 BleTermDevice（调试终端范式）

使用 Nordic UART Service（NUS）将 BLE 模拟成串口流：
```
Service UUID:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
TX Char:       6E400003-...  (ESP32 → 手机，Notify)
RX Char:       6E400002-...  (手机 → ESP32，Write)
```

指令格式与 `SerialTermDevice` 完全相同（文本行），两者共用同一个 `CommandDispatcher`。

---

## 九、文件结构（重构后）

```
src/user_interface/
│
├── user_interface.h/cpp          [MODIFY] 三子系统 + CommandDispatcher 集成
│
├── paradigm/                     [NEW] 范式抽象接口层
│   ├── i_knob_device.h
│   ├── i_touch_device.h          (含 RichCommand 定义)
│   └── i_term_device.h
│
├── command_dispatcher.h/cpp      [NEW] 富指令调度器
│
├── common/                       [KEEP] 保留通用数据类型
│   ├── display_types.h           (DisplaySnapshot 等，无变化)
│   ├── ui_intent.h               (UIIntent，无变化)
│   ├── menu_system.h/cpp         (MenuSystem，无变化)
│   └── menu_config.h/cpp         (无变化)
│
├── drv_oled_rotary/              [MODIFY] 合并为 OledKnobDevice
│   ├── oled_knob_device.h/cpp    [NEW] 实现 IKnobDevice
│   ├── oled.h/cpp                [KEEP] 硬件驱动不变
│   └── RotaryInputSource.h/cpp   [KEEP] 硬件驱动不变
│
├── drv_rs485_screen/             [MODIFY] 改造为 Rs485TouchDevice
│   └── rs485_touch_device.h/cpp  [RENAME] 产生 RichCommand，不再直接调业务
│
├── drv_mcgs/                     [MODIFY] 改造为 McgsTouchDevice
│   └── mcgs_touch_device.h/cpp   [RENAME] 同上
│
├── drv_terminal/                 [MODIFY] 改造为 SerialTermDevice
│   └── serial_term_device.h/cpp  [RENAME] 补全输入侧
│
├── drv_ble/                      [NEW] BLE 传输层
│   ├── ble_manager.h/cpp
│   ├── ble_touch_device.h/cpp    实现 ITouchDevice
│   └── ble_term_device.h/cpp     实现 ITermDevice（可选）
│
│── display.h                     [DELETE] 被 paradigm/ 接口替代
└── input_source.h                [DELETE] 被 paradigm/ 接口替代
```

---

## 十、main.cpp 重构后的注册方式

```cpp
void setupHmi() {
    auto* ui = UserInterface::getInstance();
    auto& disp = CommandDispatcher::getInstance();

    // ── 旋钮菜单子系统 ──────────────────────────────
    #if ENABLE_HMI_OLED_ROTARY
        static OledKnobDevice oledKnob;
        oledKnob.initialize();
        ui->addKnobDevice(&oledKnob);
    #endif

    // ── 触摸直控子系统 ──────────────────────────────
    #if ENABLE_HMI_RS485
        static Rs485TouchDevice rs485Touch;
        rs485Touch.initialize();
        ui->addTouchDevice(&rs485Touch);
    #endif

    #if ENABLE_HMI_MCGS
        static McgsTouchDevice mcgsTouch;
        mcgsTouch.initialize();
        ui->addTouchDevice(&mcgsTouch);
    #endif

    #if ENABLE_HMI_BLE_TOUCH
        BleManager::getInstance()->initialize("ESP32-Sorter");
        static BleTouchDevice bleTouch(BleManager::getInstance());
        bleTouch.initialize();
        ui->addTouchDevice(&bleTouch);
    #endif

    // ── 终端子系统 ──────────────────────────────────
    #if ENABLE_HMI_TERMINAL
        static SerialTermDevice serialTerm;
        serialTerm.initialize();
        ui->addTermDevice(&serialTerm);
    #endif

    #if ENABLE_HMI_BLE_TERM
        static BleTermDevice bleTerm(BleManager::getInstance());
        bleTerm.initialize();
        ui->addTermDevice(&bleTerm);
    #endif
}

void setupCommandDispatcher() {
    auto& d = CommandDispatcher::getInstance();
    d.registerHandler("set_outlet",  [](const RichCommand& c) { /* ... */ });
    d.registerHandler("set_offset",  [](const RichCommand& c) { /* ... */ });
    d.registerHandler("diag_outlet", [](const RichCommand& c) { /* ... */ });
    d.registerHandler("page_sync",   [](const RichCommand& c) { /* ... */ });
    d.registerHandler("tare",        [](const RichCommand& c) { /* ... */ });
}
```

---

## 十一、主循环变化（vUITask）

```cpp
void vUITask(void* pvParameters) {
    for (;;) {
        // 旋钮范式：产生 UIIntent，驱动菜单/App
        UIIntent intent = userInterface->getNextIntent();

        if (intent.action == UIAction::NAVIGATE_PATH)
            switchToAppType((AppType)intent.value);

        if (menuModeActive)
            handleMenuMode(intent);
        else
            updateActiveApp(currentMs, intent);

        // 触摸+终端范式：调度所有待处理的富指令/文本命令
        userInterface->dispatchPendingCommands();

        // 广播快照到所有子系统（旋钮显示器 + 触摸对端 + 终端输出）
        DisplaySnapshot snapshot = buildSnapshot();
        userInterface->broadcastSnapshot(snapshot);

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
```

---

## 十二、迁移策略（渐进式，不一次性重写）

| 阶段 | 内容 | 风险 |
|---|---|---|
| **Phase 1** | 新增 `paradigm/` 接口头文件；新增 `CommandDispatcher` | 无风险，只加文件 |
| **Phase 2** | `Rs485TouchScreen` → `Rs485TouchDevice`（改造 processLine 产生 RichCommand） | 中：需同步注册业务 Handler |
| **Phase 3** | `McgsDisplay` → `McgsTouchDevice`（同上） | 中 |
| **Phase 4** | `OLED` + `RotaryInputSource` → `OledKnobDevice`（包装合并） | 低：内部合并，外部注册方式不变 |
| **Phase 5** | `Terminal` → `SerialTermDevice`（补全输入侧） | 低 |
| **Phase 6** | 新增 `drv_ble/`（BleTouchDevice + BleTermDevice） | 低：纯新增 |
| **Phase 7** | 删除旧 `display.h` / `input_source.h`；更新 `UserInterface` 顶层 | 中：清理收尾 |

每个 Phase 独立可验证，不需要一次性全部完成。

---

## 十三、遗留问题（待确认）

> [!NOTE]
> **Q1 已决策：Façade 门面模式（外合内分）**
>
> `RotaryInputSource` 存在技术约束：两个 `IRAM_ATTR` 全局中断函数通过
> `RotaryInputSource::instance` 静态指针访问单例，**必须保留独立单例**。
> `OLED` 的 `isAvailable()` 与 `RotaryInputSource` 的可用性独立（I2C 可能 Init Failed，GPIO 中断始终可用）。
>
> **结论**：新建 `OledKnobDevice : public IKnobDevice`，内部持有两个单例的指针（不拥有），
> 外部 API 统一为一个 `IKnobDevice`，内部两个硬件单例及其 ISR 完全不动。
>
> ```cpp
> class OledKnobDevice : public IKnobDevice {
>     OLED*              _oled   = OLED::getInstance();
>     RotaryInputSource* _rotary = RotaryInputSource::getInstance();
> public:
>     void initialize()             { _oled->initialize(); _rotary->initialize(); }
>     void tick()          override { _rotary->tick(); }
>     bool hasIntent()     override { return _rotary->hasIntent(); }
>     UIIntent pollIntent()override { return _rotary->pollIntent(); }
>     void renderMenu(...) override { _oled->renderMenu(...); }
>     void refreshSnapshot(const DisplaySnapshot& s) override { _oled->refresh(s); }
>     bool isAvailable()   const override { return _oled->isAvailable(); }
>     String getName()     const override { return "OledKnob"; }
> };
> ```

> [!NOTE]
> **Q2 已决策**：`CommandDispatcher` 放在 `user_interface/` 层，由 `UserInterface` 持有，
> `main.cpp` 在 `setupCommandDispatcher()` 中注册处理器，业务 App 不直接依赖 CommandDispatcher。

> [!NOTE]
> **Q3 已决策**：`display.h` 和 `input_source.h` 在 Phase 1-6 期间保留为过渡期兼容层，Phase 7 统一删除。

> [!NOTE]
> **Q4 已决策**：`DisplaySnapshot` 无需修改，是纯业务数据容器，三种范式均可消费。

---

*文档版本：v1.1（方案 C）| 日期：2026-06-16*

---

## 十四、遗留文件清理计划（重构完成后执行）

> [!CAUTION]
> **删除前提**：必须确认生产固件在目标硬件上实际运行验证通过，所有新驱动功能正常，
> 再执行删除操作。不要仅凭编译通过就删除旧文件。

### 14.1 可完全删除的文件（已有新版替代）

| 旧文件 | 替代文件 | 说明 |
|---|---|---|
| `drv_rs485_screen/rs485_touch_screen.h` | `drv_rs485_screen/rs485_touch_device.h` | 旧驱动，直接调业务对象 |
| `drv_rs485_screen/rs485_touch_screen.cpp` | `drv_rs485_screen/rs485_touch_device.cpp` | 同上 |
| `drv_mcgs/mcgs_display.h` | `drv_mcgs/mcgs_touch_device.h` | 旧驱动，直接写 EEPROM |
| `drv_mcgs/mcgs_display.cpp` | `drv_mcgs/mcgs_touch_device.cpp` | 同上 |
| `drv_terminal/terminal.h` | `drv_terminal/serial_term_device.h` | 旧驱动，无输入侧 |
| `drv_terminal/terminal.cpp` | `drv_terminal/serial_term_device.h`（纯头文件）| 同上 |

### 14.2 可删除的抽象接口（被新范式接口取代）

| 旧文件 | 替代文件 | 说明 |
|---|---|---|
| `common/display.h` | `paradigm/i_knob_device.h` + `i_touch_device.h` | 旧 Display 抽象基类 |
| `common/input_source.h` | `paradigm/i_knob_device.h` + `i_term_device.h` | 旧 InputSource 抽象基类 |

### 14.3 需要保留的旧驱动（OledKnobDevice Façade 内部依赖）

> [!NOTE]
> `OledKnobDevice` 是 Façade 门面类，内部持有两个旧单例的指针。
> 两个旧文件**必须保留**，不可删除。

| 文件 | 保留原因 |
|---|---|
| `drv_oled_rotary/oled.h/cpp` | `OledKnobDevice` 内部持有 `OLED*` 单例 |
| `drv_oled_rotary/RotaryInputSource.h/cpp` | `OledKnobDevice` 内部持有 `RotaryInputSource*`；`IRAM_ATTR` ISR 依赖静态指针 |

### 14.4 `user_interface.h/cpp` 内部的遗留代码（向下兼容过渡层）

删除上述旧驱动文件后，可同步清理 `UserInterface` 中的旧接口：

| 成员/方法 | 类型 | 清理条件 |
|---|---|---|
| `Display* displayDevices[]` 数组 | 旧 Display 列表 | 确认无任何旧 Display 注册后删除 |
| `InputSource* inputSources[]` 数组 | 旧 InputSource 列表 | 同上 |
| `addDisplayDevice()` / `removeDisplayDevice()` | 旧注册方法 | 同上 |
| `addExternalDisplayDevice()` | 旧静态注册方法 | 同上 |
| `addInputSource()` | 旧静态注册方法 | 同上 |
| `clearAllDisplayDevices()` | 旧方法 | 同上 |
| `clearAllInputSources()` | 旧方法 | 同上 |
| `refreshAllDevices()` | 旧广播方法 | 改用 `broadcastSnapshot()` 后删除 |
| `OutputChannel` 枚举及相关方法 | 旧输出渠道控制 | 终端范式通过 `ITermDevice` 控制，删除旧渠道概念 |
| `output()` / `outputLine()` | 旧输出方法 | 同上 |
| `getEncoderDelta()` 等已弃用方法 | 标注 `[已弃用]` | 确认无引用后删除 |

### 14.5 `main.cpp` 中需要清理的旧 `#include`

删除旧驱动文件后，同步移除 `main.cpp` 中以下 include：

```cpp
// 以下三行在旧驱动文件删除后移除
#include "user_interface/drv_oled_rotary/oled.h"        // OledKnobDevice 内部已 include
#include "user_interface/drv_terminal/terminal.h"        // 替换为 serial_term_device.h
#include "user_interface/drv_rs485_screen/rs485_touch_screen.h"  // 替换为 rs485_touch_device.h
#include "user_interface/drv_mcgs/mcgs_display.h"        // 替换为 mcgs_touch_device.h
```

### 14.6 清理执行顺序建议

```
1. 硬件实测验证新驱动功能正常
2. 删除 14.1 中的旧驱动文件（6个文件）
3. 删除 14.2 中的旧抽象接口（2个文件）
4. 清理 main.cpp 中的旧 #include（4行）
5. 清理 user_interface.h/cpp 中的遗留代码（14.4 中的方法/成员）
6. 全量编译验证
7. 删除本文档的"遗留文件清理计划"章节（清理工作完成）
```

---

*文档版本：v1.2（方案 C，清理计划已记录）| 日期：2026-06-16*
