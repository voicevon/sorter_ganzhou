#include "mcgs_touch_device.h"
#include "../../main.h"

// ── 构造函数 ──────────────────────────────────────────────────────────────────

McgsTouchDevice::McgsTouchDevice()
    : _modbus(PIN_HMI_485_RX, PIN_HMI_485_TX, PIN_HMI_485_EN, HMI_MODBUS_BAUD),
      _slaveId(HMI_MODBUS_SLAVE_ID) {}

// ── 初始化 ────────────────────────────────────────────────────────────────────

void McgsTouchDevice::initialize() {
    if (_isAvailable) return;

    _modbus.begin();
    _isAvailable = true;

    Serial.println("[McgsTouchDevice] initialize() — Modbus RTU ready");

    uint16_t fwVer = 0x0200;
    _modbus.syncWrite(_slaveId, HMI_REG_FIRMWARE_VER, fwVer);
    _modbus.syncWrite(_slaveId, HMI_REG_SYS_MODE, 0);

    syncConfigToScreen();
}

// ── 上电配置同步（只读 EEPROM，写 Modbus 寄存器，无业务耦合）────────────────

void McgsTouchDevice::syncConfigToScreen() {
    if (EEPROM.read(EEPROM_ADDR_DIAMETER) != 0xAA) {
        Serial.println("[McgsTouchDevice] EEPROM not initialized, skipping sync");
        return;
    }
    Serial.println("[McgsTouchDevice] Syncing EEPROM config to screen...");

    uint8_t outlet0Mode = EEPROM.read(EEPROM_ADDR_OUTLET0_MODE);

    for (uint8_t i = 0; i < NUM_OUTLETS; i++) {
        uint8_t minVal  = EEPROM.read(EEPROM_ADDR_DIAMETER_DATA + i * 3);
        uint8_t maxVal  = EEPROM.read(EEPROM_ADDR_DIAMETER_DATA + i * 3 + 1);
        uint8_t lenMask = EEPROM.read(EEPROM_ADDR_DIAMETER_DATA + i * 3 + 2);

        _modbus.syncWrite(_slaveId, HMI_REG_OUTLET_MIN(i),     (uint16_t)minVal  * 100);
        _modbus.syncWrite(_slaveId, HMI_REG_OUTLET_MAX(i),     (uint16_t)maxVal  * 100);
        _modbus.syncWrite(_slaveId, HMI_REG_OUTLET_LENMASK(i), lenMask);

        uint16_t mode = (i == 0) ? outlet0Mode : 0;
        _modbus.syncWrite(_slaveId, HMI_REG_OUTLET_MODE(i), mode);
    }
    Serial.println("[McgsTouchDevice] Config sync done.");
}

// ── pushProductionData ────────────────────────────────────────────────────────

void McgsTouchDevice::pushProductionData(uint16_t diameter, uint16_t speedPerHour,
                                         uint32_t totalCount, uint32_t identCount) {
    if (!_isAvailable) return;
    _modbus.asyncWrite(_slaveId, HMI_REG_DIAMETER,      diameter,                          nullptr);
    _modbus.asyncWrite(_slaveId, HMI_REG_SPEED_PER_HOUR,speedPerHour,                     nullptr);
    _modbus.asyncWrite(_slaveId, HMI_REG_TOTAL_HI,      (uint16_t)(totalCount >> 16),      nullptr);
    _modbus.asyncWrite(_slaveId, HMI_REG_TOTAL_LO,      (uint16_t)(totalCount & 0xFFFF),   nullptr);
    _modbus.asyncWrite(_slaveId, HMI_REG_IDENT_HI,      (uint16_t)(identCount >> 16),      nullptr);
    _modbus.asyncWrite(_slaveId, HMI_REG_IDENT_LO,      (uint16_t)(identCount & 0xFFFF),   nullptr);
}

// ── ITouchDevice::pushSnapshot ────────────────────────────────────────────────

void McgsTouchDevice::pushSnapshot(const DisplaySnapshot& snapshot) {
    if (!_isAvailable) return;

    _modbus.asyncWrite(_slaveId, HMI_REG_SYS_MODE, (uint16_t)snapshot.currentMode, nullptr);

    if (snapshot.currentMode == APP_PRODUCTION) {
        pushProductionData(
            (uint16_t)snapshot.data.dashboard.latestDiameter,
            (uint16_t)snapshot.data.dashboard.sortingSpeedPerHour,
            0,
            (uint32_t)snapshot.data.dashboard.identifiedCount
        );
    } else if (snapshot.currentMode == APP_DIAG_ENCODER) {
        _modbus.asyncWrite(_slaveId, HMI_REG_DIAG_VAL1,
                           (uint16_t)snapshot.data.encoder.raw, nullptr);
    }
}

// ── ITouchDevice::tick ────────────────────────────────────────────────────────

void McgsTouchDevice::tick() {
    if (!_isAvailable) return;

    // 心跳
    uint32_t now = millis();
    if (now - _lastHeartbeatUpdate >= 1000) {
        _lastHeartbeatUpdate = now;
        _heartbeat++;
        _modbus.asyncWrite(_slaveId, HMI_REG_HEARTBEAT, _heartbeat, nullptr);
    }

    // 异步读 5 个控制寄存器
    _modbus.asyncRead(_slaveId, HMI_REG_CMD_CODE, 5,
        [this](Modbus::ResultCode result, uint16_t, void*) -> bool {
            if (result != Modbus::EX_SUCCESS) return false;

            uint16_t cmd     = _cmdRegs[0];
            uint16_t idx     = _cmdRegs[1];
            uint16_t field   = _cmdRegs[2];
            uint16_t value   = _cmdRegs[3];
            uint16_t confirm = _cmdRegs[4];

            if (cmd != HmiCmd::NONE)          handleCmdCode(cmd);
            if (cmd == HmiCmd::OK && idx < NUM_OUTLETS) handleParamWrite((uint8_t)idx, (uint8_t)field, value);
            if (confirm != 0) {
                handleCmdConfirm();
                _modbus.asyncWrite(_slaveId, HMI_REG_CMD_CONFIRM, 0, nullptr);
            }
            return true;
        },
        _cmdRegs);
}

// ── pollCommand ───────────────────────────────────────────────────────────────

RichCommand McgsTouchDevice::pollCommand() {
    if (_cmdQueue.empty()) return RichCommand{};
    RichCommand cmd = _cmdQueue.front();
    _cmdQueue.pop();
    return cmd;
}

// ── 内部辅助 ──────────────────────────────────────────────────────────────────

/**
 * CmdCode 1-4 → 导航类 RichCommand（由 UserInterface 转换为 UIIntent）
 * 使用约定命令字：
 *   "nav_relative" paramI=-1/+1
 *   "activate"
 *   "back"
 */
void McgsTouchDevice::handleCmdCode(uint16_t cmd) {
    if (cmd == 0) return;

    Serial.printf("[McgsTouchDevice] CmdCode=%d received\n", cmd);

    RichCommand rich;
    switch (cmd) {
        case 1: rich.cmd = "nav_relative"; rich.paramI = -1; break;  // 上
        case 2: rich.cmd = "nav_relative"; rich.paramI = +1; break;  // 下
        case 3: rich.cmd = "activate";                        break;  // 确认
        case 4: rich.cmd = "back";                            break;  // 返回
        default: return;
    }
    _cmdQueue.push(rich);

    _modbus.asyncWrite(_slaveId, HMI_REG_CMD_CODE, 0, nullptr); // 握手清零
}

/**
 * 参数写入 → "mcgs_write_param" RichCommand
 * Handler 在 main.cpp 中注册，负责写 EEPROM（驱动层不再直接操作 EEPROM）
 */
void McgsTouchDevice::handleParamWrite(uint8_t outletIdx, uint8_t field, uint16_t value) {
    if (outletIdx >= NUM_OUTLETS) return;

    RichCommand cmd;
    cmd.cmd    = "mcgs_write_param";
    cmd.index  = outletIdx;
    cmd.paramI = field;    // HmiField::MIN / MAX / LEN_MASK / MODE
    cmd.paramF = (float)value;
    _cmdQueue.push(cmd);

    Serial.printf("[McgsTouchDevice] Queued mcgs_write_param: outlet=%d field=%d value=%d\n",
                  outletIdx, field, value);
}

/**
 * EEPROM commit → "eeprom_commit" RichCommand
 * Handler 在 main.cpp 中注册，负责 EEPROM.commit()
 */
void McgsTouchDevice::handleCmdConfirm() {
    RichCommand cmd;
    cmd.cmd = "eeprom_commit";
    _cmdQueue.push(cmd);
    Serial.println("[McgsTouchDevice] Queued eeprom_commit");
}
