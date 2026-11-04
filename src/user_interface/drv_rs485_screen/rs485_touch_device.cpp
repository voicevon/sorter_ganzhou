#include "rs485_touch_device.h"
#include "../../config.h"
#include "../../main.h"

// ── 单例 ─────────────────────────────────────────────────────────────────────

Rs485TouchDevice* Rs485TouchDevice::getInstance() {
    static Rs485TouchDevice instance;
    return &instance;
}

Rs485TouchDevice::Rs485TouchDevice()
    : _serial(&Serial2), _state(RS485_TOUCH_STATE_IDLE),
      _txTimestamp(0), _slavePage("dashboard") {}

// ── 初始化 ────────────────────────────────────────────────────────────────────

void Rs485TouchDevice::initialize() {
    Serial.println("[Rs485TouchDevice] >> Initializing RS485 HMI Touch Screen driver...");
    Serial.printf("[Rs485TouchDevice] Pin Config -> RX: %d, TX: %d, EN: %d, Baud: %ld\n",
                  PIN_HMI_485_RX, PIN_HMI_485_TX, PIN_HMI_485_EN, HMI_MODBUS_BAUD);

    pinMode(PIN_HMI_485_EN, OUTPUT);
    digitalWrite(PIN_HMI_485_EN, LOW);
    _serial->begin(HMI_MODBUS_BAUD, SERIAL_8N2, PIN_HMI_485_RX, PIN_HMI_485_TX);
    _rxBuffer.reserve(512);

    Serial.println("[Rs485TouchDevice] Ready.");
}

// ── CRC8 / 帧发送 ─────────────────────────────────────────────────────────────

uint8_t Rs485TouchDevice::calculateCRC8(const char* data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
        }
    }
    return crc;
}

void Rs485TouchDevice::sendPayload(const String& jsonStr) {
    uint8_t crc = calculateCRC8(jsonStr.c_str(), jsonStr.length());
    String out = "$" + jsonStr + "*";
    if (crc < 0x10) out += "0";
    out += String(crc, HEX);
    out += "\n";

    digitalWrite(PIN_HMI_485_EN, HIGH);
    _serial->print(out);
    _serial->flush();
    digitalWrite(PIN_HMI_485_EN, LOW);

    while (_serial->available()) _serial->read(); // 清除本地回显

    _txTimestamp = millis();
    _state = RS485_TOUCH_STATE_WAITING_ACK;
    _rxBuffer = "";
}

// ── pushSnapshot（主控 → 对端）────────────────────────────────────────────────

void Rs485TouchDevice::pushSnapshot(const DisplaySnapshot& snapshot) {
    String jsonStr;
    bool hasData = false;

    if (_slavePage == "dashboard" && snapshot.currentMode == APP_PRODUCTION) {
        StaticJsonDocument<512> doc;
        doc["page"] = "dashboard";
        JsonObject data = doc.createNestedObject("data");
        data["frame_counter"] = _frameCounter++;
        data["speed"]    = snapshot.data.dashboard.sortingSpeedPerSecond;
        data["yield"]    = snapshot.data.dashboard.identifiedCount;
        data["capacity"] = snapshot.data.dashboard.sortingSpeedPerHour;
        data["diameter"] = snapshot.data.dashboard.latestDiameter;
        serializeJson(doc, jsonStr);
        hasData = true;
    }
    else if (_slavePage == "diag_encoder" &&
             (snapshot.currentMode == APP_DIAG_ENCODER ||
              snapshot.currentMode == APP_DIAG_HMI ||
              snapshot.currentMode == APP_CONFIG_PHASE_OFFSET)) {
        StaticJsonDocument<512> doc;
        doc["page"] = "diag_encoder";
        JsonObject data = doc.createNestedObject("data");
        data["raw_val"]       = snapshot.data.encoder.raw;
        data["corrected_val"] = snapshot.data.encoder.corrected;
        data["logic_val"]     = snapshot.data.encoder.logic;
        data["offset"]        = snapshot.data.encoder.offset;
        data["zero_count"]    = snapshot.data.encoder.zeroCount;
        data["zero_total"]    = snapshot.data.encoder.zeroTotal;
        data["zero_correct"]  = snapshot.data.encoder.zeroCorrect;
        data["pulse_count"]   = snapshot.data.encoder.raw;
        data["velocity"]      = (float)abs(snapshot.data.encoder.raw - _lastRawCount) * 10.0f;
        data["status"]        = 1;
        _lastRawCount = snapshot.data.encoder.raw;
        serializeJson(doc, jsonStr);
        hasData = true;
    }
    else if ((_slavePage == "diag_outlets" || _slavePage == "config_outlets") &&
             (snapshot.currentMode == APP_DIAG_OUTLET ||
              snapshot.currentMode == APP_CONFIG_DIAMETER)) {
        StaticJsonDocument<2048> doc;
        doc["page"] = _slavePage;
        JsonArray data = doc.createNestedArray("data");
        for (int i = 0; i < NUM_OUTLETS; i++) {
            JsonObject obj = data.createNestedObject();
            obj["min"]  = (float)snapshot.data.outlet.outlets[i].min;
            obj["max"]  = (float)snapshot.data.outlet.outlets[i].max;
            obj["mask"] = (uint8_t)snapshot.data.outlet.outlets[i].mask;
            if (_slavePage == "diag_outlets") {
                obj["state"] = snapshot.data.outlet.outlets[i].isOpen ? 1 : 0;
            }
        }
        serializeJson(doc, jsonStr);
        hasData = true;
    }
    else if (_slavePage == "diag_laser" && snapshot.currentMode == APP_DIAG_SCANNER) {
        StaticJsonDocument<1536> doc;
        doc["page"] = "diag_laser";
        JsonObject data = doc.createNestedObject("data");
        data["states"]       = snapshot.data.scanner.states;
        data["sample_count"] = snapshot.data.scanner.sampleCount;
        const char* keys[] = {"history_p1","history_p2","history_p3","history_p4"};
        int samples = snapshot.data.scanner.sampleCount;
        if (samples < 0) samples = 0;
        if (samples > 200) samples = 200;
        int numBytes = (samples + 7) / 8;
        for (int i = 0; i < TOTAL_SCAN_CHANNELS; i++) {
            String hex = "";
            hex.reserve(numBytes * 2 + 1);
            for (int j = 0; j < numBytes; j++) {
                char buf[3];
                sprintf(buf, "%02X", snapshot.data.scanner.history[i][j]);
                hex += buf;
            }
            data[keys[i]] = hex;
        }
        serializeJson(doc, jsonStr);
        hasData = true;
    }

    if (hasData) {
        while (_txQueue.size() >= 5) _txQueue.pop();
        _txQueue.push(jsonStr);
    }
}

// ── tick（心跳：接收 + 超时 + 发送驱动）─────────────────────────────────────

void Rs485TouchDevice::tick() {
    // 1. 接收
    while (_serial->available()) {
        char c = _serial->read();
        if (c == '\n') {
            processLine(_rxBuffer);
            _rxBuffer = "";
            _state = RS485_TOUCH_STATE_IDLE;
        } else {
            _rxBuffer += c;
            if (_rxBuffer.length() > 1024) {
                Serial.println("[Rs485TouchDevice] WARNING: RX Buffer overflow, clearing!");
                _rxBuffer = "";
            }
        }
    }

    // 2. 超时检测
    if (_state == RS485_TOUCH_STATE_WAITING_ACK) {
        if (millis() - _txTimestamp > 1000) {
            Serial.printf("[Rs485TouchDevice] RX Timeout on page [%s]\n", _slavePage.c_str());
            _state = RS485_TOUCH_STATE_IDLE;
            _rxBuffer = "";
        }
    }

    // 3. 驱动发送队列
    if (_state == RS485_TOUCH_STATE_IDLE && !_txQueue.empty()) {
        String payload = _txQueue.front();
        _txQueue.pop();
        sendPayload(payload);
    }
}

// ── pollCommand ───────────────────────────────────────────────────────────────

RichCommand Rs485TouchDevice::pollCommand() {
    if (_cmdQueue.empty()) return RichCommand{};
    RichCommand cmd = _cmdQueue.front();
    _cmdQueue.pop();
    return cmd;
}

// ── processLine（核心：JSON 解析 → RichCommand，不调任何业务对象）────────────

void Rs485TouchDevice::processLine(const String& line) {
    String clean = line;
    clean.trim();

    if (!clean.startsWith("$")) return;

    int starIdx = clean.lastIndexOf('*');
    if (starIdx < 1) return;

    String jsonStr  = clean.substring(1, starIdx);
    String crcHex   = clean.substring(starIdx + 1);
    uint8_t calcCrc = calculateCRC8(jsonStr.c_str(), jsonStr.length());
    uint8_t recvCrc = (uint8_t)strtol(crcHex.c_str(), NULL, 16);

    if (calcCrc != recvCrc) {
        Serial.printf("[Rs485TouchDevice] CRC mismatch! Calc:%02X Recv:%02X\n", calcCrc, recvCrc);
        return;
    }

    StaticJsonDocument<2048> doc;
    if (deserializeJson(doc, jsonStr) != DeserializationError::Ok) return;

    // ── 1. 页面同步 ──────────────────────────────────────────────────────────
    if (doc.containsKey("page")) {
        String newPage = doc["page"].as<String>();
        if (newPage != _slavePage) {
            _slavePage = newPage;

            // 映射页面名 → AppType（业务常量），产生 page_sync 富指令
            RichCommand cmd;
            cmd.cmd      = "page_sync";
            cmd.strParam = newPage;

            if      (newPage == "dashboard")      cmd.paramI = 0; // APP_PRODUCTION
            else if (newPage == "diag_encoder")   cmd.paramI = 1; // APP_DIAG_ENCODER
            else if (newPage == "diag_laser")     cmd.paramI = 2; // APP_DIAG_SCANNER
            else if (newPage == "diag_outlets")   cmd.paramI = 3; // APP_DIAG_OUTLET
            else if (newPage == "config_outlets") cmd.paramI = 4; // APP_CONFIG_DIAMETER
            else if (newPage == "about")          cmd.paramI = 5; // APP_VERSION_INFO
            else cmd.paramI = -1; // 未知页面，Handler 自行处理

            _cmdQueue.push(cmd);
            Serial.printf("[Rs485TouchDevice] Page sync -> '%s' (paramI=%d)\n",
                          newPage.c_str(), cmd.paramI);
        }
    }

    // ── 2. 事件列表 ──────────────────────────────────────────────────────────
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
                cmd.index  = ev["index"] | 0;
                cmd.paramF  = ev["min"]  | 0.0f;
                cmd.paramF2 = ev["max"]  | 0.0f;
                cmd.paramI  = ev["mask"] | 0;
                needSaveConfig = true;

            } else if (evCmd == "set_offset") {
                // 兼容 "state" 和 "params" 两种字段名
                cmd.paramI = ev.containsKey("state") ? (int)ev["state"] : (int)ev["params"];

            } else {
                // 透传未知命令，让 CommandDispatcher 决定处理
                cmd.paramI = ev["params"] | 0;
            }

            _cmdQueue.push(cmd);
            Serial.printf("[Rs485TouchDevice] Event -> cmd='%s', index=%d\n",
                          cmd.cmd.c_str(), cmd.index);
        }

        // 批量 set_outlet 完成后统一触发一次持久化保存
        if (needSaveConfig) {
            RichCommand saveCmd;
            saveCmd.cmd = "save_config";
            _cmdQueue.push(saveCmd);
        }
    }
}
