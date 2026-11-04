#include "app_config.h"
#include "../user_interface/common/display_types.h"
#include "../modular/encoder.h"
#include "../config.h"

// =========================
// AppConfigDiameter 实现
// =========================

void AppConfigDiameter::initializeMode() {
    currentSubMode = 0;
    uiState = STATE_SELECTOR;
    encoderAccumulator = 0;
    refreshDisplay();
}

void AppConfigDiameter::update(uint32_t currentMs, bool btnPressed) {
    // 自愈刷新（由 RS485 Touch UI 的 set_outlet 指令更新配置，captureSnapshot 推送状态）
    if (currentMs - lastRefreshMs >= 250) {
        lastRefreshMs = currentMs;
        refreshDisplay();
    }
    (void)btnPressed;
}

void AppConfigDiameter::refreshDisplay() {
    // 显示已由 DisplaySnapshot 统一托管
}

void AppConfigDiameter::handleValueChange(int delta) {
    int totalItems = NUM_OUTLETS + 2;
    if (uiState == STATE_SELECTOR) {
        currentSubMode = (currentSubMode + delta) % totalItems;
        if (currentSubMode < 0) currentSubMode += totalItems;
    } else {
        int targetOutlet = currentSubMode - 1;
        if (uiState == STATE_EDIT_MAX) {
            int val = sorter->getOutletMaxDiameter(targetOutlet);
            sorter->setOutletMaxDiameter(targetOutlet, constrain(val + delta, 0, 255));
        } else if (uiState == STATE_EDIT_MIN) {
            int val = sorter->getOutletMinDiameter(targetOutlet);
            sorter->setOutletMinDiameter(targetOutlet, constrain(val + delta, 0, 255));
        } else if (uiState == STATE_EDIT_LENGTH) {
            uint8_t current = sorter->getOutletTargetLength(targetOutlet);
            int next = (int)current + (delta > 0 ? 1 : -1);
            if (next > 7) next = 1;
            else if (next < 1) next = 7;
            sorter->setOutletTargetLength(targetOutlet, (uint8_t)next);
        }
    }
    refreshDisplay();
}

// =========================
// AppConfigPhaseOffset 实现
// =========================

void AppConfigPhaseOffset::initializeMode() {
    editingOffset = Encoder::getInstance()->getPhaseOffset();
    encoderAccumulator = 0;
    refreshDisplay();
}

void AppConfigPhaseOffset::update(uint32_t currentMs, bool btnPressed) {
    editingOffset = Encoder::getInstance()->getPhaseOffset();
    if (currentMs - lastRefreshMs >= 300) {
        lastRefreshMs = currentMs;
        refreshDisplay();
    }
    (void)btnPressed;
}

void AppConfigPhaseOffset::handleValueChange(int delta) {
    editingOffset = (editingOffset + delta + ENCODER_MAX_PHASE) % ENCODER_MAX_PHASE;
    refreshDisplay();
}

void AppConfigPhaseOffset::refreshDisplay() {
    // 显示已由 DisplaySnapshot 统一托管
}

void AppConfigDiameter::captureSnapshot(DisplaySnapshot& snapshot) {
    snapshot.currentMode = APP_CONFIG_DIAMETER;
    strcpy(snapshot.activePage, "config_outlets");
    
    for (int i = 0; i < 8; i++) {
        if (i < NUM_OUTLETS) {
            snapshot.data.outlet.outlets[i].min = sorter->getOutletMinDiameter(i);
            snapshot.data.outlet.outlets[i].max = sorter->getOutletMaxDiameter(i);
            snapshot.data.outlet.outlets[i].mask = sorter->getOutletTargetLength(i);
            snapshot.data.outlet.outlets[i].isOpen = sorter->getOutlet(i)->isPositionOpen();
        } else {
            snapshot.data.outlet.outlets[i].min = 0;
            snapshot.data.outlet.outlets[i].max = 0;
            snapshot.data.outlet.outlets[i].mask = 0;
            snapshot.data.outlet.outlets[i].isOpen = false;
        }
    }
    snapshot.data.outlet.activeOutletIndex = currentSubMode - 1;
    snapshot.data.outlet.subMode = (int)uiState;
    snapshot.data.outlet.cycleCount = 0;
}

void AppConfigPhaseOffset::captureSnapshot(DisplaySnapshot& snapshot) {
    snapshot.currentMode = APP_CONFIG_PHASE_OFFSET;
    strcpy(snapshot.activePage, "config_phase_offset");
    
    snapshot.data.encoder.raw = Encoder::getInstance()->getRawCount();
    snapshot.data.encoder.corrected = Encoder::getInstance()->getZeroCrossRawCount();
    snapshot.data.encoder.logic = Encoder::getInstance()->getCurrentPosition();
    snapshot.data.encoder.zeroCount = Encoder::getInstance()->getZeroCrossCount() - Encoder::getInstance()->getForcedZeroCount();
    snapshot.data.encoder.zeroCorrect = Encoder::getInstance()->getZeroCrossCount();
    snapshot.data.encoder.zeroTotal = Encoder::getInstance()->getForcedZeroCount();
    snapshot.data.encoder.offset = editingOffset;
}
