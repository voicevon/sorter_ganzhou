#ifndef BLE_TOUCH_DEVICE_H
#define BLE_TOUCH_DEVICE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <queue>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include "../paradigm/i_touch_device.h"
#include "ble_manager.h"

// ── 二进制状态包页面 ID ───────────────────────────────────────────────────────
enum BlePageId : uint8_t {
    BLE_PAGE_DASHBOARD    = 0x01,
    BLE_PAGE_ENC_DIAG     = 0x02,
    BLE_PAGE_SCANNER_DIAG = 0x03,
    BLE_PAGE_OUTLET_DIAG  = 0x04,
};

class BleTouchDevice : public ITouchDevice {
public:
    BleTouchDevice() : _ble(BleManager::getInstance()) {}

    // ── ITouchDevice 接口 ────────────────────────────────────────────────────
    void initialize() override;
    bool isAvailable() const override { return _ble.isConnected(); }
    String getName() const override { return "BleTouchDevice"; }

    void tick() override;
    void pushSnapshot(const DisplaySnapshot& snapshot) override;

    bool hasCommand() override;
    RichCommand pollCommand() override;

private:
    BleManager& _ble;
    std::queue<RichCommand> _cmdQueue;
    mutable portMUX_TYPE _queueMux = portMUX_INITIALIZER_UNLOCKED;

    // 上次推送的快照内容（用于脏检测）
    DisplaySnapshot _lastSnapshot;
    bool            _isFirstPush  = true;
    uint32_t        _lastPushMs   = 0;
    static const uint32_t KEEPALIVE_INTERVAL_MS = 2000;

    // ── 序列化辅助 ─────────────────────────────────────────────────────────
    bool serializeSnapshot(const DisplaySnapshot& snap, uint8_t* buf, size_t& outLen);
    void parseJson(const String& json);
    uint8_t calcChecksum(const uint8_t* data, size_t len);
};

#endif // BLE_TOUCH_DEVICE_H
