#include "ble_manager.h"

// ── GATT Server 回调类 ────────────────────────────────────────────────────────

class BleServerCallbacks : public BLEServerCallbacks {
public:
    BleServerCallbacks(BleManager* mgr) : _mgr(mgr) {}

    void onConnect(BLEServer* pServer) override {
        _mgr->_onConnect();
    }
    void onDisconnect(BLEServer* pServer) override {
        _mgr->_onDisconnect();
    }
private:
    BleManager* _mgr;
};

// ── COMMAND 特征值写入回调 ─────────────────────────────────────────────────────

class BleCommandCallbacks : public BLECharacteristicCallbacks {
public:
    BleCommandCallbacks(BleManager* mgr) : _mgr(mgr) {}

    void onWrite(BLECharacteristic* pChar) override {
        String value = pChar->getValue().c_str();
        value.trim();
        if (value.length() > 0) {
            _mgr->_onCommandReceived(value);
        }
    }
private:
    BleManager* _mgr;
};

// ── 安全配对回调（Passkey Entry）─────────────────────────────────────────────

class BleSecurityCallbacks : public BLESecurityCallbacks {
public:
    BleSecurityCallbacks(BleManager* mgr) : _mgr(mgr) {}

    // ESP32 作为显示方：生成 PIN 码并通过回调显示
    uint32_t onPassKeyRequest() override {
        return _mgr->_onPinRequested();
    }

    void onPassKeyNotify(uint32_t pass_key) override {
        // ESP32 输出 PIN，对端需在其设备上输入
        Serial.printf("[BleManager] *** PAIRING PIN: %06d ***\n", pass_key);
        _mgr->_onPinRequested(); // 触发用户侧显示（如 OLED）
    }

    bool onConfirmPIN(uint32_t pass_key) override {
        // Numeric Comparison：自动确认（若需要手动确认可改为 false 并等待用户输入）
        Serial.printf("[BleManager] Confirm PIN: %06d -> Auto-accepted\n", pass_key);
        return true;
    }

    bool onSecurityRequest() override {
        return true;
    }

    void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) override {
        if (auth_cmpl.success) {
            Serial.println("[BleManager] Pairing SUCCESS");
        } else {
            Serial.printf("[BleManager] Pairing FAILED, reason: 0x%02X\n",
                          auth_cmpl.fail_reason);
        }
    }

private:
    BleManager* _mgr;
};

// ── BleManager 实现 ───────────────────────────────────────────────────────────

void BleManager::initialize(const String& deviceName) {
    Serial.printf("[BleManager] Initializing as Peripheral: '%s'\n", deviceName.c_str());

    BLEDevice::init(deviceName.c_str());

    // 安全配对设置
    setupSecurity();

    // 创建 GATT Server
    _pServer = BLEDevice::createServer();
    _pServer->setCallbacks(new BleServerCallbacks(this));

    // 创建 Service
    BLEService* pService = _pServer->createService(BLE_SERVICE_UUID);

    // STATUS 特征值（Read + Notify）
    _pStatusChar = pService->createCharacteristic(
        BLE_STATUS_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ   |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    _pStatusChar->addDescriptor(new BLE2902());

    // COMMAND 特征值（Write + WriteNoResp）
    _pCommandChar = pService->createCharacteristic(
        BLE_COMMAND_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE           |
        BLECharacteristic::PROPERTY_WRITE_NR
    );
    _pCommandChar->setCallbacks(new BleCommandCallbacks(this));

    // 启动 Service
    pService->start();

    // 开始广播
    startAdvertising();

    Serial.println("[BleManager] BLE Peripheral ready, advertising...");
}

void BleManager::setupSecurity() {
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
    BLEDevice::setSecurityCallbacks(new BleSecurityCallbacks(this));

    BLESecurity* pSecurity = new BLESecurity();
    // Passkey Entry：ESP32 输出 PIN，对端输入
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    pSecurity->setCapability(ESP_IO_CAP_OUT);          // ESP32 仅输出 PIN
    pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
}

void BleManager::startAdvertising() {
    BLEAdvertising* pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(BLE_SERVICE_UUID);
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);
    pAdv->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
}

bool BleManager::sendNotify(const uint8_t* data, size_t length) {
    if (!_connected || !_pStatusChar) return false;
    _pStatusChar->setValue(const_cast<uint8_t*>(data), length);
    _pStatusChar->notify();
    return true;
}

// ── 内部事件处理 ──────────────────────────────────────────────────────────────

void BleManager::_onConnect() {
    _connected    = true;
    _wasConnected = true;
    Serial.println("[BleManager] Client connected");
}

void BleManager::_onDisconnect() {
    _connected = false;
    Serial.println("[BleManager] Client disconnected, restarting advertising...");
    // 断连后自动重新广播
    delay(200);
    BLEDevice::startAdvertising();
}

void BleManager::_onCommandReceived(const String& json) {
    if (_cmdCallback) {
        _cmdCallback(json);
    }
}

uint32_t BleManager::_onPinRequested() {
    // 生成固定 PIN（可改为随机）
    static uint32_t pin = 123456;
    Serial.printf("[BleManager] Passkey: %06d\n", pin);
    if (_pinCallback) {
        _pinCallback(pin);
    }
    return pin;
}
