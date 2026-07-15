#ifndef I_TOUCH_DEVICE_H
#define I_TOUCH_DEVICE_H

#include <Arduino.h>
#include "../common/display_types.h"

/**
 * @struct RichCommand
 * @brief 触摸范式的富语义指令
 *
 * 承载触摸屏/手机 App 发来的复杂操作语义，
 * 替代驱动层直接调用业务对象的架构妥协。
 *
 * 由驱动层（ITouchDevice 实现）产生，
 * 由 CommandDispatcher 路由到对应的业务 Handler。
 *
 * 命令字典（cmd 字段）：
 *   "page_sync"   - 对端切换页面，strParam = 页面名称
 *   "set_outlet"  - 配置出口参数，index=出口号, paramF=min, paramF2=max, paramI=mask
 *   "diag_outlet" - 手动控制出口, index=出口号, paramB=true(开)/false(关)
 *   "set_offset"  - 设置编码器相位偏移, paramI=新偏移值
 *   "tare"        - 清零/去皮, paramI=参数
 *   "menu_click"  - 点击菜单项（旋钮范式兼容），paramI=目标 AppType
 *   "save_config" - 持久化配置到 EEPROM（无参数）
 */
struct RichCommand {
    String   cmd;       // 命令标识符
    int      index;     // 对象索引（出口号、通道号等），默认 -1
    float    paramF;    // 主浮点参数（如 min 直径）
    float    paramF2;   // 副浮点参数（如 max 直径）
    int      paramI;    // 整型参数（偏移量、状态值等）
    bool     paramB;    // 布尔参数（开/关）
    String   strParam;  // 字符串参数（页面名称等）

    // 默认构造：空命令
    RichCommand()
        : cmd(""), index(-1), paramF(0.0f), paramF2(0.0f),
          paramI(0), paramB(false), strParam("") {}

    // 命令是否有效
    bool isValid() const { return cmd.length() > 0; }
};

/**
 * @interface ITouchDevice
 * @brief 触摸直控范式设备接口
 *
 * 代表任何具有"页面直接操控"能力的 HMI 设备：
 *   - 对端有自己的 UI 渲染引擎（LVGL / HTML / Native App）
 *   - 主控推送 DisplaySnapshot → 对端渲染
 *   - 对端产生 RichCommand → 主控通过 CommandDispatcher 处理
 *
 * 典型实现：
 *   - Rs485TouchDevice （RS485 协议触摸屏）
 *   - McgsTouchDevice  （Modbus RTU 昆仑通态屏）
 *   - BleTouchDevice   （BLE 手机 App）
 */
class ITouchDevice {
public:
    virtual ~ITouchDevice() = default;

    /**
     * @brief 初始化硬件/通讯
     */
    virtual void initialize() = 0;

    /**
     * @brief 心跳驱动（在 UI 任务主循环中调用）
     * 用于轮询接收缓冲区、处理超时、驱动发送队列等。
     */
    virtual void tick() = 0;

    /**
     * @brief 推送状态快照（主控 → 对端显示）
     * 驱动层负责将 snapshot 序列化为对端能理解的格式（JSON / 二进制 / Modbus 寄存器）并发送。
     * 允许内部节流（如有内容变化或超时才实际发送）。
     */
    virtual void pushSnapshot(const DisplaySnapshot& snapshot) = 0;

    /**
     * @brief 是否有待处理的富指令（非破坏性检查）
     */
    virtual bool hasCommand() = 0;

    /**
     * @brief 消费并返回一条富指令
     * @return RichCommand，若无指令则返回 isValid()==false 的空命令
     */
    virtual RichCommand pollCommand() = 0;

    /**
     * @brief 设备是否处于连接/可用状态
     */
    virtual bool isAvailable() const = 0;

    /**
     * @brief 设备名称（调试用）
     */
    virtual String getName() const = 0;
};

#endif // I_TOUCH_DEVICE_H
