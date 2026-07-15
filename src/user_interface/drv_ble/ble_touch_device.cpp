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

// ── tick（目前 BLE 采用回调模式，tick 无需额外轮询）────────────────────────

void BleTouchDevice::tick() {
    // BleManager 的 COMMAND 回调是在 BLE 任务上下文中异步触发的。
    // parseJson() 已在回调中直接将 RichCommand 放入 _cmdQueue。
    // 若未来需要在 UI 任务上下文中处理，可将 JSON 先缓存到线程安全队列，此处消费。
    // 当前版本：直接依赖 Arduino BLE 栈的单线程回调，ESP32 BLE 回调在 BT 任务，
    // std::queue 操作在同一核可接受（无并发竞争）。
}

// ── pollCommand ───────────────────────────────────────────────────────────────

RichCommand BleTouchDevice::pollCommand() {
    if (_cmdQueue.empty()) return RichCommand{};
    RichCommand cmd = _cmdQueue.front();
    _cmdQueue.pop();
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
            // speed×100(u32) | speed/min(u16) | identified(u16) |
            // trays(u16) | diameter×100(i16) | scan_count(u8)  = 13B
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
            // raw(i16) corrected(i16) logic(i16) zeroCount(i16)
            // zeroCorrect(i16) zeroTotal(i16) offset(i16) = 14B
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
            // active_outlet(u8) sub_mode(u8) cycle_count(u32) = 6B
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
    // checksum = XOR(buf[0..2]) XOR XOR(payload)
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
        _cmdQueue.push(cmd);
    }

    // ── 2. 事件列表（与 Rs485TouchDevice::processLine 完全对称）──────────────
    if (doc.containsKey("events")) {
        JsonArray events = doc["events"];
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
            } else if (evCmd == "set_offset") {
                cmd.paramI = ev.containsKey("state")
                           ? (int)ev["state"] : (int)ev["params"];
            } else {
                cmd.paramI = ev["params"] | 0;
            }
            _cmdQueue.push(cmd);
        }
    }
}
