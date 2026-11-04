#include "user_interface.h"
#include "main.h"

UserInterface* UserInterface::instance = nullptr;

UserInterface::UserInterface() {
    instance = this;
    for (int i = 0; i < MAX_KNOB_DEVICES; i++)  knobDevices[i]  = nullptr;
    for (int i = 0; i < MAX_TOUCH_DEVICES; i++) touchDevices[i] = nullptr;
    for (int i = 0; i < MAX_TERM_DEVICES; i++)  termDevices[i]  = nullptr;
}

UserInterface* UserInterface::getInstance() {
    if (instance == nullptr) {
        instance = new UserInterface();
    }
    return instance;
}

void UserInterface::initialize() {
    // 硬件初始化由 main.cpp 中的 setupHmi() 完成
}

void UserInterface::renderMenu(MenuNode* node, int cursorIndex, int scrollOffset) {
    for (int i = 0; i < knobDeviceCount; i++) {
        knobDevices[i]->renderMenu(node, cursorIndex, scrollOffset);
    }
}

bool UserInterface::isDisplayAvailable() const {
    for (int i = 0; i < touchDeviceCount; i++) {
        if (touchDevices[i]->isAvailable()) return true;
    }
    for (int i = 0; i < knobDeviceCount; i++) {
        if (knobDevices[i]->isAvailable()) return true;
    }
    for (int i = 0; i < termDeviceCount; i++) {
        if (termDevices[i]->isAvailable()) return true;
    }
    return false;
}

void UserInterface::clearDisplay() {
    for (int i = 0; i < knobDeviceCount; i++) {
        knobDevices[i]->clearDisplay();
    }
}

UIIntent UserInterface::getNextIntent() {
    // 优先级1：来自触摸/终端范式的导航指令（由 dispatchPendingCommands 推入）
    if (!_intentQueue.empty()) {
        UIIntent intent = _intentQueue.front();
        _intentQueue.pop();
        return intent;
    }

    // 优先级2：旋钮范式设备
    for (int i = 0; i < knobDeviceCount; i++) {
        if (knobDevices[i]->hasIntent()) {
            return knobDevices[i]->pollIntent();
        }
    }

    return UIIntent(UIAction::NONE);
}

// ── 三范式子系统：设备注册 ────────────────────────────────────────────────────

void UserInterface::addKnobDevice(IKnobDevice* dev) {
    if (!dev || knobDeviceCount >= MAX_KNOB_DEVICES) return;
    for (int i = 0; i < knobDeviceCount; i++) if (knobDevices[i] == dev) return;
    knobDevices[knobDeviceCount++] = dev;
    Serial.printf("[UI] KnobDevice added: %s (total=%d)\n", dev->getName().c_str(), knobDeviceCount);
}

void UserInterface::addTouchDevice(ITouchDevice* dev) {
    if (!dev || touchDeviceCount >= MAX_TOUCH_DEVICES) return;
    for (int i = 0; i < touchDeviceCount; i++) if (touchDevices[i] == dev) return;
    touchDevices[touchDeviceCount++] = dev;
    Serial.printf("[UI] TouchDevice added: %s (total=%d)\n", dev->getName().c_str(), touchDeviceCount);
}

void UserInterface::addTermDevice(ITermDevice* dev) {
    if (!dev || termDeviceCount >= MAX_TERM_DEVICES) return;
    for (int i = 0; i < termDeviceCount; i++) if (termDevices[i] == dev) return;
    termDevices[termDeviceCount++] = dev;
    Serial.printf("[UI] TermDevice added: %s (total=%d)\n", dev->getName().c_str(), termDeviceCount);
}

// ── broadcastSnapshot：广播快照到所有子系统 ───────────────────────────────────

void UserInterface::broadcastSnapshot(const DisplaySnapshot& snapshot) {
    static DisplaySnapshot lastSnapshot;
    static bool isFirst = true;
    static uint32_t lastRefreshMs = 0;

    bool forceRefresh = (millis() - lastRefreshMs >= 500);
    if (!isFirst && !forceRefresh && memcmp(&snapshot, &lastSnapshot, sizeof(DisplaySnapshot)) == 0) {
        return; // 500ms 内数据无变化则跳过，节省带宽
    }

    isFirst = false;
    lastRefreshMs = millis();
    memcpy(&lastSnapshot, &snapshot, sizeof(DisplaySnapshot));

    // 1. 旋钮设备：刷新快照
    for (int i = 0; i < knobDeviceCount; i++) {
        knobDevices[i]->refreshSnapshot(snapshot);
    }

    // 2. 触摸设备：推送快照到对端（如 RS485 触摸屏）
    for (int i = 0; i < touchDeviceCount; i++) {
        touchDevices[i]->pushSnapshot(snapshot);
    }

    // 3. 终端设备：ASCII 输出快照
    for (int i = 0; i < termDeviceCount; i++) {
        termDevices[i]->pushSnapshot(snapshot);
    }
}

// ── dispatchPendingCommands：调度所有待处理指令 ───────────────────────────────

void UserInterface::dispatchPendingCommands() {
    auto& dispatcher = CommandDispatcher::getInstance();

    // ── 1. 触摸设备 tick + 指令路由 ─────────────────────────────────────────
    for (int i = 0; i < touchDeviceCount; i++) {
        touchDevices[i]->tick();
        while (touchDevices[i]->hasCommand()) {
            RichCommand cmd = touchDevices[i]->pollCommand();
            if (!cmd.isValid()) continue;

            // 导航类命令 → UIIntent 队列
            if (cmd.cmd == "nav_relative") {
                _intentQueue.push(UIIntent(UIAction::NAVIGATE_RELATIVE, cmd.paramI));
            } else if (cmd.cmd == "activate") {
                _intentQueue.push(UIIntent(UIAction::ACTIVATE));
            } else if (cmd.cmd == "back") {
                _intentQueue.push(UIIntent(UIAction::BACK));
            } else if (cmd.cmd == "page_sync" && cmd.paramI >= 0) {
                _intentQueue.push(UIIntent(UIAction::NAVIGATE_PATH, cmd.paramI));
            } else {
                // 业务指令 → CommandDispatcher
                dispatcher.dispatch(cmd);
            }
        }
    }

    // ── 2. 终端设备 tick + 文本命令路由 ─────────────────────────────────────
    for (int i = 0; i < termDeviceCount; i++) {
        termDevices[i]->tick();
        while (termDevices[i]->hasCommand()) {
            String line = termDevices[i]->pollCommand();
            if (line.length() == 0) continue;

            if      (line == "ACTIVATE") _intentQueue.push(UIIntent(UIAction::ACTIVATE));
            else if (line == "BACK")     _intentQueue.push(UIIntent(UIAction::BACK));
            else if (line == "CANCEL")   _intentQueue.push(UIIntent(UIAction::CANCEL));
            else if (line.startsWith("NAV:")) {
                int delta = line.substring(4).toInt();
                _intentQueue.push(UIIntent(UIAction::NAVIGATE_RELATIVE, delta));
            } else {
                dispatcher.dispatchText(line);
            }
        }
    }

    // ── 3. 旋钮设备 tick ────────────────────────────────────────────────────
    for (int i = 0; i < knobDeviceCount; i++) {
        knobDevices[i]->tick();
    }
}
