# BLE 通讯集成设计文档 v2.0

**适用项目**：`esp32_sorter`  
**模块路径**：`src/user_interface/drv_ble/`  
**前置阅读**：`drv_rs485_screen/rs485_touch_screen.h/cpp`  

---

## 一、问题重述：两种完全不同的 UI 交互范式

研究 `drv_rs485_screen` 和 `esp32_sorter_screen` 后，发现现有架构存在一个根本性的**分裂**：

| 维度 | 旋钮菜单范式 | 触摸屏范式 |
|---|---|---|
| **硬件** | 物理旋钮编码器 + 按钮 | 触摸屏（本地 LVGL / 远程手机） |
| **交互模型** | 线性导航树（NAVIGATE_RELATIVE → ACTIVATE）| 直接操控（点击 Tab / 修改具体参数） |
| **状态同步** | 主控推送 `DisplaySnapshot` → 驱动渲染 | **双向**：主控推数据 ↔ 对端直接切页/发指令 |
| **"页"的概念** | 不存在，只有菜单游标 `cursorIndex` | 存在独立页面（`dashboard`/`diag_encoder`/... |
| **典型例子** | `drv_oled_rotary` + `RotaryInputSource` | `drv_rs485_screen`（同时实现 `Display` + `InputSource`）|

**手机通过 BLE 连接，本质上是"远程触摸屏"**，行为与 `drv_rs485_screen` 几乎完全一致，而不是与 OLED 一致。  
v1.0 方案把 BLE 拆成两个独立的 `BleDisplay` + `BleInput` 是错的，正确模式是**一个类同时实现两个接口**（与 `Rs485TouchScreen` 完全对称）。

---

## 二、架构诊断：`UIIntent` 是否够用？

`UIIntent` 目前能表达的事件：

```cpp
enum class UIAction {
    NONE, NAVIGATE_RELATIVE, NAVIGATE_PATH, ACTIVATE, CANCEL, BACK, SET_VALUE
};
```

对于旋钮范式，这已足够。  
对于触摸屏范式，`rs485_touch_screen.cpp` 中存在**绕过 UIIntent 的直接操作**：

```cpp
// 直接调用业务对象，不经过 UIIntent
appOutletDiag.setManualOutletState(index, state == 1);
sorter.setOutletMinDiameter(index, finalMin);
Encoder::getInstance()->setPhaseOffset(newOffset);
```

这说明：**触摸屏范式的事件颗粒度超出了 UIIntent 的表达能力。**  
`rs485_touch_screen.cpp` 目前的解法是在驱动层直接 `include` 业务模块头文件（`encoder.h`、`sorter.h`、`app_outlet_diag.h`），这是一个**架构妥协**，导致驱动层与业务层强耦合。

---

## 三、重构方向建议（供讨论）

### 方案 A：最小改动，BLE 对齐 RS485（推荐先采用）

**不改动现有架构，BLE 驱动仿照 `Rs485TouchScreen` 实现。**

- `BleScreen` 类同时实现 `Display` + `InputSource`
- JSON 通讯协议与 RS485 侧**共享**（手机侧和 RS485 屏侧协议完全一致）
- RS485 现有的架构妥协（驱动直接调业务）照搬到 BLE 侧
- **优点**：改动范围极小，可快速验证 BLE 可行性
- **缺点**：`BleScreen.cpp` 会和 `Rs485TouchScreen.cpp` 一样耦合业务层

### 方案 B：引入「富指令总线」重构（中期建议）

在 `UIIntent` 旁边增加一个 `RichCommand` 通道，专门承载触摸屏范式的复杂指令，将驱动层与业务层解耦：

```
旋钮输入  ──→  UIIntent (导航意图)    ──→  菜单系统 / App
触屏/BLE  ──→  RichCommand (富指令)   ──→  CommandDispatcher ──→ 各业务对象
```

`RichCommand` 示例：
```cpp
struct RichCommand {
    String cmd;          // "set_outlet", "set_offset", "diag_outlet"
    int    index;        // 对象索引（出口号等）
    float  paramF;       // 浮点参数（min/max）
    int    paramI;       // 整型参数
    bool   paramB;       // 布尔参数
};
```

`CommandDispatcher` 由业务层注册具体处理函数，驱动层只负责产生 `RichCommand`，不再 `include` 业务模块。

- **优点**：架构清晰，驱动与业务彻底解耦，可复用
- **缺点**：改动范围较大，需同步改造 `Rs485TouchScreen`

### 方案 C：全面重构 UserInterface（激进，长期）

将 `UserInterface` 拆解为两个独立的子系统：

```
UserInterface
├── KnobUI          (旋钮菜单子系统：InputSource → MenuSystem → Display)
└── TouchscreenUI   (触摸屏子系统：RichEvent ↔ PageSync ↔ Display)
    ├── 本地 LVGL 屏幕
    ├── RS485 远程屏
    └── BLE 手机/平板
```

- **优点**：从根本上分离两种交互范式，结构最清晰
- **缺点**：改动最大，main.cpp 和所有 App 都需要适配

---

## 四、BLE 具体技术设计（独立于上述方案，均适用）

### 4.1 通讯物理层

- ESP32 作为 **BLE Peripheral（GATT Server）**
- 手机/PC/硬件作为 **BLE Central（GATT Client）**
- 安全：**BLE 配对绑定**，Passkey Entry（ESP32 生成 6 位 PIN）

### 4.2 GATT 服务设计

```
Service UUID: A1B2C3D4-E5F6-7890-ABCD-EF1234567800

STATUS  (Read + Notify)  ...7801   主控 → 对端  状态推送（二进制）
COMMAND (Write)          ...7802   对端 → 主控  JSON 指令（与 RS485 侧共享格式）
```

### 4.3 COMMAND 指令格式（复用 RS485 协议）

对端写入 JSON 字符串（无帧头帧尾，BLE 自带完整性保证）：

```json
{"page": "dashboard"}
{"events": [{"cmd": "set_outlet", "index": 2, "min": 3.5, "max": 6.0, "mask": 3}]}
{"events": [{"cmd": "set_offset", "state": 12}]}
{"events": [{"cmd": "diag_outlet", "index": 0, "state": 1}]}
{"events": [{"cmd": "menu_click", "params": 3}]}
```

> 与 RS485 侧完全一致 → 手机 App 和 RS485 屏侧可共用同一套后端解析逻辑

### 4.4 STATUS 推送格式（二进制）

```
[magic: 0xA5][page_id: 1B][payload_len: 1B][checksum: XOR][payload: NB]
```

page_id 枚举：

```cpp
enum BlePageId : uint8_t {
    BLE_PAGE_DASHBOARD    = 0x01,
    BLE_PAGE_ENC_DIAG     = 0x02,
    BLE_PAGE_SCANNER_DIAG = 0x03,
    BLE_PAGE_OUTLET_DIAG  = 0x04,
};
```

Dashboard payload（12 字节）：速度×100(u32) | 速度/分(u16) | 产量(u16) | 托盘(u16) | 直径×100(i16) | 根数(u8) = 13 bytes total

> 总包体 ≤ 20B，低于 BLE 最小 MTU 23B，无需分包

### 4.5 推送策略

```
每次 refresh(snapshot) 被调用：
  if (已连接 && (数据有变化 || 距上次推送 > 2000ms)) {
      序列化 → Notify
  }
```

### 4.6 PIN 码显示

配对 PIN 通过 `Serial` 输出。若系统已挂载 OLED，可额外在 OLED 显示。  
`BleManager::initialize()` 接收一个可选的 `Display*` 参数（默认 `nullptr`）。

---

## 五、文件结构（以方案 A 为例）

```
src/user_interface/drv_ble/
├── ble_manager.h/cpp    BLE Stack 单例（配对、GATT Server、收发）
└── ble_screen.h/cpp     同时实现 Display + InputSource
                         仿照 Rs485TouchScreen，解析 JSON 指令 + 发送二进制状态
```

`main.cpp` 集成：

```cpp
#if ENABLE_HMI_BLE
    BleManager::getInstance()->initialize("ESP32-Sorter");
    BleScreen* bleScreen = BleScreen::getInstance();
    bleScreen->initialize();
    userInterface->addDisplayDevice(bleScreen);
    userInterface->addInputSource(bleScreen);
#endif
```

---

## 六、待讨论决策

> [!IMPORTANT]
> **决策 1 - 先做哪个方案？**
> - 方案 A（快速验证 BLE 可行性，接受临时耦合）
> - 方案 B（同步引入 RichCommand 解耦，改造量中等）
> - 方案 C（全面重构，改造量大）

> [!NOTE]
> **决策 2 - RS485 与 BLE 是否共用一套 JSON 解析器？**
> 如果共用，可以把 `Rs485TouchScreen::processLine()` 中的 JSON 解析逻辑抽取为一个公共函数/类，
> `BleScreen` 和 `Rs485TouchScreen` 都调用它，避免代码重复。

> [!NOTE]
> **决策 3 - 手机 App 端由谁开发？**
> 如果是 Web App（Web Bluetooth API），则 JSON 文本协议更友好；
> 如果是原生 App 或 nRF Connect 调试，二进制协议更高效。
> 当前方案已选择：COMMAND 用 JSON，STATUS 用二进制，两端各取所长。

---

*文档版本：v2.0 | 日期：2026-06-16*
