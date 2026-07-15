#ifndef SERIAL_TERM_DEVICE_H
#define SERIAL_TERM_DEVICE_H

#include <Arduino.h>
#include <queue>
#include "../paradigm/i_term_device.h"

/**
 * @class SerialTermDevice
 * @brief USB Serial 终端设备（重构版，补全输入侧）
 *
 * 继承 ITermDevice，实现键盘终端范式。
 * 相比旧版 Terminal（只有输出，无输入），新增：
 *   - tick()：轮询 Serial.read()，将字节流拼接为命令行
 *   - hasCommand() / pollCommand()：消费命令行队列
 *
 * 命令行格式（行协议，与 BleTermDevice 共用同一解析约定）：
 *   ACTION[:VALUE]\n
 *   示例：
 *     ACTIVATE\n
 *     NAV:+1\n
 *     set_offset:12\n
 *     save_config\n
 *
 * 命令行由 CommandDispatcher::dispatchText() 解析并路由到业务 Handler。
 */
class SerialTermDevice : public ITermDevice {
public:
    SerialTermDevice() : _isAvailable(false) {}

    // ── ITermDevice 接口 ─────────────────────────────────────────────────────

    void initialize() override {
        // Serial 通常已在 setup() 中 begin()，此处仅标记可用
        _isAvailable = true;
        _rxBuffer.reserve(256);
        Serial.println("[SerialTermDevice] Terminal ready. Type 'help' for commands.");
    }

    /**
     * @brief 轮询 Serial 字节流，拼接完整命令行
     * 每次调用在主循环中执行，逐字节读取直到 '\n'。
     */
    void tick() override {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                if (_rxBuffer.length() > 0) {
                    _rxBuffer.trim();
                    if (_rxBuffer.length() > 0) {
                        _cmdQueue.push(_rxBuffer);
                        Serial.printf("[SerialTermDevice] CMD received: '%s'\n",
                                      _rxBuffer.c_str());
                    }
                    _rxBuffer = "";
                }
            } else {
                _rxBuffer += c;
                if (_rxBuffer.length() > 512) {
                    Serial.println("[SerialTermDevice] WARNING: Line too long, clearing buffer");
                    _rxBuffer = "";
                }
            }
        }
    }

    void print(const String& text) override {
        Serial.print(text);
    }

    void println(const String& text) override {
        Serial.println(text);
    }

    /**
     * @brief 将状态快照以 ASCII 文本格式输出到 Serial
     * 内部节流（2000ms 最多输出一次）
     */
    void pushSnapshot(const DisplaySnapshot& snapshot) override {
        uint32_t now = millis();
        if (now - _lastSnapshotMs < 2000) return;
        _lastSnapshotMs = now;

        Serial.printf("\n[TERM] Mode:%d | Page:%s\n",
                      snapshot.currentMode, snapshot.activePage);

        switch (snapshot.currentMode) {
            case APP_PRODUCTION:
                Serial.printf("  Spd:%.1f/s  %d/min  %d/h\n",
                    snapshot.data.dashboard.sortingSpeedPerSecond,
                    snapshot.data.dashboard.sortingSpeedPerMinute,
                    snapshot.data.dashboard.sortingSpeedPerHour);
                Serial.printf("  Ident:%d  Trays:%d  LastDia:%dmm\n",
                    snapshot.data.dashboard.identifiedCount,
                    snapshot.data.dashboard.transportedTrayCount,
                    snapshot.data.dashboard.latestDiameter);
                break;
            case APP_DIAG_ENCODER:
                Serial.printf("  Raw:%d  Corr:%d  Logic:%d  Offset:%d\n",
                    snapshot.data.encoder.raw,
                    snapshot.data.encoder.corrected,
                    snapshot.data.encoder.logic,
                    snapshot.data.encoder.offset);
                break;
            case APP_DIAG_SCANNER:
                Serial.printf("  LaserStates:0x%02X  Samples:%d\n",
                    snapshot.data.scanner.states,
                    snapshot.data.scanner.sampleCount);
                break;
            case APP_DIAG_OUTLET:
            case APP_CONFIG_DIAMETER:
                Serial.printf("  ActiveOutlet:%d  SubMode:%d  Cycles:%u\n",
                    snapshot.data.outlet.activeOutletIndex,
                    snapshot.data.outlet.subMode,
                    snapshot.data.outlet.cycleCount);
                break;
            default:
                break;
        }
    }

    bool hasCommand() override { return !_cmdQueue.empty(); }

    String pollCommand() override {
        if (_cmdQueue.empty()) return "";
        String cmd = _cmdQueue.front();
        _cmdQueue.pop();
        return cmd;
    }

    bool isAvailable() const override { return _isAvailable; }

    String getName() const override { return "SerialTermDevice"; }

private:
    bool     _isAvailable;
    String   _rxBuffer;
    uint32_t _lastSnapshotMs = 0;
    std::queue<String> _cmdQueue;
};

#endif // SERIAL_TERM_DEVICE_H
