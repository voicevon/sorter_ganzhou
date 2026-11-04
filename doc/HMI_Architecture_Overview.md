# HMI 交互系统架构说明

此文档描述了 ESP32 Sorter 项目中用户界面与输入系统的组织逻辑。

> **硬件支持范围说明**：当前硬件配置**仅包含自研 RS485 触摸屏**（`ENABLE_HMI_RS485 = 1`）。OLED 显示器、物理旋转编码器按键、MCGS Modbus 屏及串口终端等其他 HMI 硬件配置均不再支持。

## 1. 核心组件
系统 HMI 层由以下核心类协同工作：
- **Rs485TouchDevice**: 底层驱动，负责通过 UART2 (RS485 半双工，带 DE/RE 方向控制与 CRC8 校验) 与触摸屏通信，将接收到的 JSON 事件解析为 `RichCommand`，并将主控的 `DisplaySnapshot` 序列化为 JSON 推送给触摸屏。
- **CommandDispatcher**: 富指令调度器，将来自 RS485 触摸屏的业务操作指令（如 `set_outlet`、`diag_outlet`、`set_offset`、`tare`）路由到 `main.cpp` 注册的业务处理器。
- **UserInterface**: 顶层门面管理器，负责管理触摸设备、调度待处理指令（`dispatchPendingCommands()`）以及向触摸屏广播状态快照（`broadcastSnapshot()`）。

## 2. 交互逻辑流 (Event Flow)

### 2.1 页面切换与模式同步 (`page_sync`)
1. 用户在 RS485 触摸屏上切换页面（如 `dashboard`、`diag_encoder`、`diag_laser`、`diag_outlets`、`config_outlets`、`about`）。
2. 触摸屏通过 RS485 发送包含 `"page"` 字段的 JSON 帧。
3. `Rs485TouchDevice::processLine()` 校验 CRC8 后生成 `page_sync` 富指令。
4. `UserInterface::dispatchPendingCommands()` 将其转换为 `UIIntent(UIAction::NAVIGATE_PATH, targetMode)`。
5. `vUITask` 调用 `switchToAppType()` 切换主控当前激活的业务 App。

### 2.2 业务参数设置与诊断控制
1. 用户在触摸屏上修改出口参数或点击诊断按钮。
2. 触摸屏发送包含 `"events"` 数组的 JSON 帧（如 `set_outlet`、`diag_outlet`、`set_offset`）。
3. `Rs485TouchDevice` 将事件压入指令队列 `_cmdQueue`。
4. `UserInterface::dispatchPendingCommands()` 取出 `RichCommand` 并交由 `CommandDispatcher::dispatch()` 执行对应的业务回调（更新 `Sorter` / `Encoder` 并持久化至 EEPROM）。

### 2.3 状态快照推送 (`DisplaySnapshot`)
1. `vUITask` 每 100ms 调用当前激活 App 的 `captureSnapshot(snapshot)`。
2. `UserInterface` 将快照推送给 `Rs485TouchDevice::pushSnapshot(snapshot)`。
3. `Rs485TouchDevice` 根据从机当前所在页面打包对应的 JSON 数据帧（格式：`$JSON*CRC8\n`），在 RS485 总线空闲时发送至触摸屏。
