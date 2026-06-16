#ifndef MCGS_TOUCH_DEVICE_H
#define MCGS_TOUCH_DEVICE_H

#include <Arduino.h>
#include <EEPROM.h>
#include <queue>
#include "../paradigm/i_touch_device.h"
#include "../../utils/ModbusMaster.h"
#include "hmi_registers.h"
#include "../../config.h"

/**
 * @class McgsTouchDevice
 * @brief 昆仑通态 TPC7062Ti (800×480) Modbus RTU 触摸屏驱动（重构版）
 *
 * 继承 ITouchDevice，实现触摸直控范式。
 * 与旧版 McgsDisplay 的核心区别：
 *   - handleCmdCode()    → 产生导航类 RichCommand（不再直接填 UIIntent）
 *   - handleParamWrite() → 产生 "mcgs_write_param" RichCommand（不再直接写 EEPROM）
 *   - handleCmdConfirm() → 产生 "eeprom_commit" RichCommand（不再直接 commit）
 *   - 业务 Handler 在 main.cpp::setupCommandDispatcher() 中注册
 *
 * 注：MCGS 的导航命令（上/下/确认/返回）是简单的 Knob 式导航，
 *     在 UserInterface::dispatchPendingCommands() 中会被识别并转换为 UIIntent。
 */
class McgsTouchDevice : public ITouchDevice {
public:
    McgsTouchDevice();
    ~McgsTouchDevice() override = default;

    // ── ITouchDevice 接口 ────────────────────────────────────────────────────
    void initialize() override;
    bool isAvailable() const override { return _isAvailable; }
    String getName() const override { return "McgsTouchDevice"; }

    void tick() override;
    void pushSnapshot(const DisplaySnapshot& snapshot) override;

    bool hasCommand() override { return !_cmdQueue.empty(); }
    RichCommand pollCommand() override;

    // ── MCGS 专用接口（在 main.cpp 任务中调用）──────────────────────────────
    /**
     * @brief 上电将 EEPROM 出口配置同步到屏幕寄存器
     * 必须在 initialize() 之后调用。
     */
    void syncConfigToScreen();

    /**
     * @brief 推送实时生产数据到仪表盘寄存器
     * @param diameter      实时直径（整数，单位 mm×100，如 2250 = 22.50mm）
     * @param speedPerHour  分拣速度（pcs/h）
     * @param totalCount    总计数
     * @param identCount    识别计数
     */
    void pushProductionData(uint16_t diameter, uint16_t speedPerHour,
                            uint32_t totalCount, uint32_t identCount);

private:
    ModbusMaster _modbus;
    bool         _isAvailable = false;
    uint8_t      _slaveId;

    uint16_t _ledBitmap = 0;
    uint16_t _cmdRegs[5] = {0}; // CmdCode, ParamIdx, ParamField, ParamValue, CmdConfirm

    uint16_t _heartbeat = 0;
    uint32_t _lastHeartbeatUpdate = 0;

    std::queue<RichCommand> _cmdQueue;

    // ── 内部辅助 ─────────────────────────────────────────────────────────────
    void handleCmdCode(uint16_t cmd);
    void handleParamWrite(uint8_t outletIdx, uint8_t field, uint16_t value);
    void handleCmdConfirm();
};

#endif // MCGS_TOUCH_DEVICE_H
