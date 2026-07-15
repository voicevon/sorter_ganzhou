#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLECharacteristic.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <functional>

// ── UUID 定义 ─────────────────────────────────────────────────────────────────
// Service
#define BLE_SERVICE_UUID        "A1B2C3D4-E5F6-7890-ABCD-EF1234567800"
// STATUS：主控 → 对端（Read + Notify，二进制状态包）
#define BLE_STATUS_CHAR_UUID    "A1B2C3D4-E5F6-7890-ABCD-EF1234567801"
// COMMAND：对端 → 主控（Write，JSON 指令）
#define BLE_COMMAND_CHAR_UUID   "A1B2C3D4-E5F6-7890-ABCD-EF1234567802"

/**
 * @class BleManager
 * @brief BLE Stack 管理单例
 *
 * 负责：
 *   1. BLE Peripheral 初始化（GATT Server + 广播）
 *   2. 安全配对（Passkey Entry，ESP32 输出 PIN）
 *   3. STATUS 特征值的 Notify 推送（二进制）
 *   4. COMMAND 特征值的接收回调
 *   5. 连接/断连事件管理
 *
 * 使用方式：
 *   BleManager::getInstance().initialize("ESP32-Sorter");
 *   BleManager::getInstance().setCommandCallback([](const String& json) { ... });
 */
class BleManager {
public:
    using CommandCallback = std::function<void(const String& json)>;
    using PinCallback     = std::function<void(uint32_t pin)>;

    static BleManager& getInstance() {
        static BleManager instance;
        return instance;
    }

    BleManager(const BleManager&)            = delete;
    BleManager& operator=(const BleManager&) = delete;

    /**
     * @brief 初始化 BLE Stack
     * @param deviceName  BLE 广播名称（如 "ESP32-Sorter"）
     */
    void initialize(const String& deviceName);

    /**
     * @brief 注册 COMMAND 接收回调
     * 每当对端写入 COMMAND 特征值时，以 JSON 字符串形式回调。
     */
    void setCommandCallback(CommandCallback cb) { _cmdCallback = cb; }

    /**
     * @brief 注册 PIN 码显示回调（配对时调用）
     * 业务层可在此将 PIN 输出到 OLED 或 Serial。
     */
    void setPinCallback(PinCallback cb) { _pinCallback = cb; }

    /**
     * @brief 向已连接的对端推送二进制状态包（Notify）
     * @param data   数据指针
     * @param length 数据字节数（≤ MTU-3，通常 ≤ 20 字节）
     * @return true = 推送成功，false = 无连接或特征值未就绪
     */
    bool sendNotify(const uint8_t* data, size_t length);

    /**
     * @brief 是否有对端连接
     */
    bool isConnected() const { return _connected; }

    // ── 内部回调（供 GATT Callback 类使用）──────────────────────────────────
    void _onConnect();
    void _onDisconnect();
    void _onCommandReceived(const String& json);
    uint32_t _onPinRequested();

private:
    BleManager() = default;

    BLEServer*         _pServer         = nullptr;
    BLECharacteristic* _pStatusChar     = nullptr;
    BLECharacteristic* _pCommandChar    = nullptr;
    bool               _connected       = false;
    bool               _wasConnected    = false;

    CommandCallback _cmdCallback;
    PinCallback     _pinCallback;

    void setupSecurity();
    void startAdvertising();
};

#endif // BLE_MANAGER_H
