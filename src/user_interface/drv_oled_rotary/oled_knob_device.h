#ifndef OLED_KNOB_DEVICE_H
#define OLED_KNOB_DEVICE_H

#include <Arduino.h>
#include "../paradigm/i_knob_device.h"
#include "oled.h"
#include "RotaryInputSource.h"

/**
 * @class OledKnobDevice
 * @brief 旋钮菜单 HMI 单元（Façade 门面模式）
 *
 * 将 OLED（I2C 显示）和 RotaryInputSource（GPIO 编码器+按钮）
 * 聚合为一个 IKnobDevice 接口对象。
 *
 * 设计决策（Q1 Façade 方案）：
 *   - 内部持有两个已有单例的指针，不拥有生命周期
 *   - RotaryInputSource 的 IRAM_ATTR ISR 全局函数及其静态指针完全不改动
 *   - OLED::isAvailable() 独立反映 I2C 初始化状态
 *   - main.cpp 只需 addKnobDevice(&oledKnob)，一行注册
 *
 * 使用方式（main.cpp）：
 *   static OledKnobDevice oledKnob;
 *   oledKnob.initialize();
 *   userInterface->addKnobDevice(&oledKnob);
 */
class OledKnobDevice : public IKnobDevice {
public:
    OledKnobDevice()
        : _oled(OLED::getInstance()),
          _rotary(RotaryInputSource::getInstance()) {}

    // ── IKnobDevice 接口 ─────────────────────────────────────────────────────

    /**
     * @brief 初始化 OLED（I2C）和旋转编码器（attachInterrupt）
     */
    void initialize() override {
        _oled->initialize();
        _rotary->initialize();
    }

    /**
     * @brief 心跳（RotaryInputSource 基于中断，无需额外轮询）
     */
    void tick() override {
        _rotary->tick();
    }

    bool hasIntent() override {
        return _rotary->hasIntent();
    }

    UIIntent pollIntent() override {
        return _rotary->pollIntent();
    }

    /**
     * @brief 渲染菜单树到 OLED
     */
    void renderMenu(MenuNode* node, int cursor, int scroll) override {
        _oled->renderMenu(node, cursor, scroll);
    }

    /**
     * @brief 刷新数据快照到 OLED（App 叶节点页面）
     */
    void refreshSnapshot(const DisplaySnapshot& snapshot) override {
        _oled->refresh(snapshot);
    }

    /**
     * @brief 清除 OLED 屏幕
     */
    void clearDisplay() override {
        _oled->clearDisplay();
    }

    /**
     * @brief OLED 是否初始化成功（I2C 设备可能不在线）
     */
    bool isAvailable() const override {
        return _oled->isAvailable();
    }

    String getName() const override {
        return "OledKnobDevice";
    }

private:
    OLED*              _oled;    // 内部单例，不拥有生命周期，ISR 链路不变
    RotaryInputSource* _rotary;  // 内部单例，IRAM_ATTR ISR 通过 instance 指针访问，不变
};

#endif // OLED_KNOB_DEVICE_H
