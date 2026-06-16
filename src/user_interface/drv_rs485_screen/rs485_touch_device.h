#ifndef RS485_TOUCH_DEVICE_H
#define RS485_TOUCH_DEVICE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <queue>
#include "../paradigm/i_touch_device.h"  // ITouchDevice + RichCommand

/**
 * @class Rs485TouchDevice
 * @brief RS485 触摸屏驱动（重构版）
 *
 * 继承 ITouchDevice，实现触摸直控范式。
 * 与旧版 Rs485TouchScreen 的核心区别：
 *   - processLine() 不再直接调用 sorter / encoder 等业务对象
 *   - 将触摸事件解析为 RichCommand，放入队列
 *   - 业务处理由 CommandDispatcher + Handler 完成（在 main.cpp 注册）
 *
 * 通讯帧格式（与旧版完全兼容）：
 *   TX: $JSON*CRC8HEX\n
 *   RX: $JSON*CRC8HEX\n
 */

// 通讯状态（使用新名避免与 rs485_touch_screen.h 中旧定义冲突）
enum Rs485TouchCommState {
    RS485_TOUCH_STATE_IDLE,
    RS485_TOUCH_STATE_WAITING_ACK
};

class Rs485TouchDevice : public ITouchDevice {
public:
    static Rs485TouchDevice* getInstance();

    // ── ITouchDevice 接口 ────────────────────────────────────────────────────
    void initialize() override;
    bool isAvailable() const override { return true; }
    String getName() const override { return "Rs485TouchDevice"; }

    void tick() override;
    void pushSnapshot(const DisplaySnapshot& snapshot) override;

    bool hasCommand() override { return !_cmdQueue.empty(); }
    RichCommand pollCommand() override;

private:
    Rs485TouchDevice();
    ~Rs485TouchDevice() = default;

    HardwareSerial* _serial;
    String _rxBuffer;

    Rs485TouchCommState _state;
    uint32_t _txTimestamp;
    uint32_t _frameCounter = 0;

    std::queue<RichCommand> _cmdQueue;   // 富指令队列（替代旧版 _intentQueue）
    std::queue<String>      _txQueue;    // 发送队列

    String _slavePage;   // 从机当前显示页面
    long   _lastRawCount = 0;

    // ── 内部辅助 ─────────────────────────────────────────────────────────────
    void    processLine(const String& line);
    uint8_t calculateCRC8(const char* data, size_t len);
    void    sendPayload(const String& jsonStr);
};

#endif // RS485_TOUCH_DEVICE_H
