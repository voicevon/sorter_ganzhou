#ifndef I_TERM_DEVICE_H
#define I_TERM_DEVICE_H

#include <Arduino.h>
#include "../common/display_types.h"

/**
 * @interface ITermDevice
 * @brief 键盘终端范式设备接口
 *
 * 代表任何基于"文本命令行"的 HMI 通道：
 *   - 输出：ASCII 文本流（不需要图形渲染）
 *   - 输入：命令行字符串（以 \n 结尾的文本行）
 *   - 适合调试、远程监控、自动化测试场景
 *
 * 典型实现：
 *   - SerialTermDevice  （USB Serial 本地终端）
 *   - BleTermDevice     （BLE UART / nRF UART Service）
 *
 * 文本命令格式（行协议）：
 *   ACTION[:VALUE]\n
 *
 *   常用命令：
 *     ACTIVATE         → UIAction::ACTIVATE
 *     BACK             → UIAction::BACK
 *     CANCEL           → UIAction::CANCEL
 *     NAV:+1           → UIAction::NAVIGATE_RELATIVE, +1
 *     NAV:-3           → UIAction::NAVIGATE_RELATIVE, -3
 *     SET:42           → UIAction::SET_VALUE, 42
 *     PATH:3           → UIAction::NAVIGATE_PATH, 3
 *     set_offset:12    → RichCommand("set_offset", paramI=12)
 *     save_config      → RichCommand("save_config")
 *
 * 注：简单导航命令（ACTIVATE/NAV等）由 CommandDispatcher::dispatchText()
 *     解析后可以产生 UIIntent 或 RichCommand，具体由 Handler 决定。
 */
class ITermDevice {
public:
    virtual ~ITermDevice() = default;

    /**
     * @brief 初始化终端通道（串口波特率、BLE 服务等）
     */
    virtual void initialize() = 0;

    /**
     * @brief 心跳驱动（在 UI 任务主循环中调用）
     * 用于轮询接收缓冲区，将字节流拼接为完整命令行。
     */
    virtual void tick() = 0;

    /**
     * @brief 向终端输出文本（不换行）
     */
    virtual void print(const String& text) = 0;

    /**
     * @brief 向终端输出文本（带换行）
     */
    virtual void println(const String& text) = 0;

    /**
     * @brief 推送状态快照（将结构化数据转为 ASCII 文本输出）
     * 驱动层自行决定格式（如每 2s 输出一行状态摘要）。
     * 允许内部节流。
     */
    virtual void pushSnapshot(const DisplaySnapshot& snapshot) = 0;

    /**
     * @brief 是否有待处理的文本命令行（非破坏性检查）
     */
    virtual bool hasCommand() = 0;

    /**
     * @brief 消费并返回一条完整命令行（已去除 \r\n，trim 过）
     * @return 命令字符串，若无命令则返回空字符串
     */
    virtual String pollCommand() = 0;

    /**
     * @brief 设备是否可用（如 BLE 未连接则不可用）
     */
    virtual bool isAvailable() const = 0;

    /**
     * @brief 设备名称（调试用）
     */
    virtual String getName() const = 0;
};

#endif // I_TERM_DEVICE_H
