#include "ble_touch_device.h"
#include "../../main.h"   // AppType 枚举

// ── 初始化 ────────────────────────────────────────────────────────────────────

void BleTouchDevice::initialize() {
    // 注册 JSON 指令接收回调到 BleManager
    _ble.setCommandCallback([this](const String& json) {
        parseJson(json);
    });
    Serial.println("[BleTouchDevice] Ready. Waiting for BLE connection...");
}

// ── tick ──────────────────────────────────────────────────────────────────────

void BleTouchDevice::tick() {
    // JSON 解析由 BLE 回调触发，通过 _queueMux 自旋锁保证跨任务队列安全
}

// ── hasCommand / pollCommand ──────────────────────────────────────────────────

bool BleTouchDevice::hasCommand() {
    portENTER_CRITICAL(&_queueMux);
    bool nonEmpty = !_cmdQueue.empty();
    portEXIT_CRITICAL(&_queueMux);
    return nonEmpty;
}

RichCommand BleTouchDevice::pollCommand() {
    RichCommand cmd{};
    portENTER_CRITICAL(&_queueMux);
    if (!_cmdQueue.empty()) {
        cmd = _cmdQueue.front();
        _cmdQueue.pop();
    }
    portEXIT_CRITICAL(&_queueMux);
    return cmd;
}

// ── pushSnapshot：序列化 DisplaySnapshot → 二进制帧 → Notify ─────────────────

void BleTouchDevice::pushSnapshot(const DisplaySnapshot& snapshot) {
    if (!_ble.isConnected()) return;

    uint32_t now = millis();
    bool changed = _isFirstPush ||
                   (memcmp(&snapshot, &_lastSnapshot, sizeof(DisplaySnapshot)) != 0);
    bool keepalive = (now - _lastPushMs) >= KEEPALIVE_INTERVAL_MS;

    if (!changed && !keepalive) return;

    uint8_t buf[32];
    size_t  len = 0;

    if (serializeSnapshot(snapshot, buf, len)) {
        if (_ble.sendNotify(buf, len)) {
            memcpy(&_lastSnapshot, &snapshot, sizeof(DisplaySnapshot));
            _isFirstPush = false;
            _lastPushMs  = now;
        }
    }
}

// ── serializeSnapshot：生成二进制状态包 ──────────────────────────────────────
// 帧格式：[0xA5][page_id][payload_len][checksum] [payload...]

bool BleTouchDevice::serializeSnapshot(const DisplaySnapshot& snap,
                                       uint8_t* buf, size_t& outLen) {
    uint8_t  pageId  = 0;
    uint8_t  payload[24];
    uint8_t  payloadLen = 0;

    switch (snap.currentMode) {
        case APP_PRODUCTION: {
            pageId = BLE_PAGE_DASHBOARD;
            uint32_t spd100 = (uint32_t)(snap.data.dashboard.sortingSpeedPerSecond * 100.0f);
            uint16_t spdMin = (uint16_t)snap.data.dashboard.sortingSpeedPerMinute;
            uint16_t ident  = (uint16_t)snap.data.dashboard.identifiedCount;
            uint16_t trays  = (uint16_t)snap.data.dashboard.transportedTrayCount;
            int16_t  diam   = (int16_t)(snap.data.dashboard.latestDiameter);
            uint8_t  scan   = (uint8_t)snap.data.dashboard.latestScanCount;

            memcpy(payload + 0, &spd100, 4);
            memcpy(payload + 4, &spdMin, 2);
            memcpy(payload + 6, &ident,  2);
            memcpy(payload + 8, &trays,  2);
            memcpy(payload + 10, &diam,  2);
            payload[12] = scan;
            payloadLen  = 13;
            break;
        }
        case APP_DIAG_ENCODER:
        case APP_DIAG_HMI:
        case APP_CONFIG_PHASE_OFFSET: {
            pageId = BLE_PAGE_ENC_DIAG;
            int16_t vals[7] = {
                (int16_t)snap.data.encoder.raw,
                (int16_t)snap.data.encoder.corrected,
                (int16_t)snap.data.encoder.logic,
                (int16_t)snap.data.encoder.zeroCount,
                (int16_t)snap.data.encoder.zeroCorrect,
                (int16_t)snap.data.encoder.zeroTotal,
                (int16_t)snap.data.encoder.offset,
            };
            memcpy(payload, vals, 14);
            payloadLen = 14;
            break;
        }
        case APP_DIAG_SCANNER: {
            pageId = BLE_PAGE_SCANNER_DIAG;
            payload[0] = snap.data.scanner.states;
            payload[1] = (uint8_t)snap.data.scanner.sampleCount;
            payloadLen = 2;
            break;
        }
        case APP_DIAG_OUTLET:
        case APP_CONFIG_DIAMETER: {
            pageId = BLE_PAGE_OUTLET_DIAG;
            payload[0] = (uint8_t)snap.data.outlet.activeOutletIndex;
            payload[1] = (uint8_t)snap.data.outlet.subMode;
            uint32_t cycles = snap.data.outlet.cycleCount;
            memcpy(payload + 2, &cycles, 4);
            payloadLen = 6;
            break;
        }
        default:
            return false; // 未知模式，不推送
    }

    // 拼帧：[0xA5][pageId][payloadLen][checksum][payload...]
    buf[0] = 0xA5;
    buf[1] = pageId;
    buf[2] = payloadLen;
    uint8_t cs = buf[0] ^ buf[1] ^ buf[2];
    for (int i = 0; i < payloadLen; i++) cs ^= payload[i];
    buf[3] = cs;
    memcpy(buf + 4, payload, payloadLen);

    outLen = 4 + payloadLen;
    return true;
}

uint8_t BleTouchDevice::calcChecksum(const uint8_t* data, size_t len) {
    uint8_t cs = 0;
    for (size_t i = 0; i < len; i++) cs ^= data[i];
    return cs;
}

// ── parseJson：解析 COMMAND JSON → RichCommand（与 Rs485 协议完全一致）────────

void BleTouchDevice::parseJson(const String& json) {
    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        Serial.printf("[BleTouchDevice] JSON parse error: %s\n", json.c_str());
        return;
    }

    // ── 1. 页面同步 ──────────────────────────────────────────────────────────
    if (doc.containsKey("page")) {
        String page = doc["page"].as<String>();
        RichCommand cmd;
        cmd.cmd      = "page_sync";
        cmd.strParam = page;
        if      (page == "dashboard")      cmd.paramI = 0;
        else if (page == "diag_encoder")   cmd.paramI = 1;
        else if (page == "diag_laser")     cmd.paramI = 2;
        else if (page == "diag_outlets")   cmd.paramI = 3;
        else if (page == "config_outlets") cmd.paramI = 4;
        else if (page == "about")          cmd.paramI = 5;
        else cmd.paramI = -1;
        portENTER_CRITICAL(&_queueMux);
        _cmdQueue.push(cmd);
        portEXIT_CRITICAL(&_queueMux);
    }

    // ── 2. 事件列表（与 Rs485TouchDevice::processLine 完全对称）──────────────
    if (doc.containsKey("events")) {
        JsonArray events = doc["events"];
        bool needSaveConfig = false;
        for (JsonObject ev : events) {
            String evCmd = ev["cmd"] | "";
            if (evCmd.length() == 0) continue;

            RichCommand cmd;
            cmd.cmd = evCmd;

            if (evCmd == "tare") {
                cmd.paramI = ev["params"] | 0;
            } else if (evCmd == "menu_click") {
                cmd.paramI = ev["params"] | 0;
            } else if (evCmd == "diag_outlet") {
                cmd.index  = ev["index"] | 0;
                cmd.paramI = ev["state"] | 0;
                cmd.paramB = (cmd.paramI == 1);
            } else if (evCmd == "set_outlet") {
                cmd.index  = ev["index"]  | 0;
                cmd.paramF  = ev["min"]   | 0.0f;
                cmd.paramF2 = ev["max"]   | 0.0f;
                cmd.paramI  = ev["mask"]  | 0;
                needSaveConfig = true;
            } else if (evCmd == "set_offset") {
                cmd.paramI = ev.containsKey("state")
                           ? (int)ev["state"] : (int)ev["params"];
            } else {
                cmd.paramI = ev["params"] | 0;
            }
            portENTER_CRITICAL(&_queueMux);
            _cmdQueue.push(cmd);
            portEXIT_CRITICAL(&_queueMux);
        }

        if (needSaveConfig) {
            RichCommand saveCmd;
            saveCmd.cmd = "save_config";
            portENTER_CRITICAL(&_queueMux);
            _cmdQueue.push(saveCmd);
            portEXIT_CRITICAL(&_queueMux);
        }
    }
}
