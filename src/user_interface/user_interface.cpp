#include "user_interface.h"
#include "main.h"

// 初始化静态实例指针
UserInterface* UserInterface::instance = nullptr;

// 私有构造函数实现
UserInterface::UserInterface() {
    instance = this; // 立即赋值，防止 addInputSource 内部触发递归
    
    // 初始化显示设备数组
    displayDeviceCount = 0;
    for (int i = 0; i < MAX_DISPLAY_DEVICES; i++) {
        displayDevices[i] = nullptr;
    }
    
    // 初始化输入设备数组
    inputSourceCount = 0;
    for (int i = 0; i < MAX_INPUT_SOURCES; i++) {
        inputSources[i] = nullptr;
    }
    
    // 不再默认添加物理旋钮，改为在 main.cpp 中显式添加
    
    // 初始化输出渠道（默认禁用所有输出渠道，需要在setup()中显式启用）
    outputChannels = 0;
    
    // 初始化语言（默认英语）
    currentLanguage = LANGUAGE_ENGLISH;
    
    // 初始化上次更新时间
    lastUpdateTime = 0;
}

// 检查是否可以更新显示
bool UserInterface::isUpdateReady() const {
    return millis() - lastUpdateTime >= UPDATE_INTERVAL;
}

// 更新上次更新时间
void UserInterface::updateLastUpdateTime() {
    lastUpdateTime = millis();
}

// 获取单例实例
UserInterface* UserInterface::getInstance() {
    if (instance == nullptr) {
        instance = new UserInterface();
    }
    return instance;
}

// 初始化所有 UI 状态
void UserInterface::initialize() {
    // 不再初始化硬件，硬件初始化移至 main.cpp
    // 不再创建显示设备实例，改为由外部注入
}

// 添加显示设备的静态方法（允许外部注入显示设备）
void UserInterface::addExternalDisplayDevice(Display* display) {
    // 获取UserInterface实例，将显示设备添加到数组中
    getInstance()->addDisplayDevice(display);
}

// 添加输入源的静态方法
void UserInterface::addInputSource(InputSource* source) {
    if (source == nullptr) return;
    
    UserInterface* ui = getInstance();
    if (ui->inputSourceCount >= MAX_INPUT_SOURCES) return;
    
    // 检查重复
    for (int i = 0; i < ui->inputSourceCount; i++) {
        if (ui->inputSources[i] == source) return;
    }
    
    ui->inputSources[ui->inputSourceCount++] = source;
}

void UserInterface::refreshAllDevices(const DisplaySnapshot& snapshot) {
    static DisplaySnapshot lastSnapshot;
    static bool isFirst = true;
    static uint32_t lastRefreshMs = 0;
    
    bool forceRefresh = (millis() - lastRefreshMs >= 500);
    
    if (!isFirst && !forceRefresh && memcmp(&snapshot, &lastSnapshot, sizeof(DisplaySnapshot)) == 0) {
        return; // No changes detected and within 500ms, swallow the refresh to save bandwidth
    }
    
    isFirst = false;
    lastRefreshMs = millis();
    memcpy(&lastSnapshot, &snapshot, sizeof(DisplaySnapshot));

    for (int i = 0; i < displayDeviceCount; i++) {
        displayDevices[i]->refresh(snapshot);
    }
}

void UserInterface::renderMenu(MenuNode* node, int cursorIndex, int scrollOffset) {
    for (int i = 0; i < displayDeviceCount; i++) {
        displayDevices[i]->renderMenu(node, cursorIndex, scrollOffset);
    }
}


// 检查显示器是否可用
bool UserInterface::isDisplayAvailable() const {
    // 如果有任何显示设备可用，则返回true
    for (int i = 0; i < displayDeviceCount; i++) {
        if (displayDevices[i]->isAvailable()) {
            return true;
        }
    }
    return false;
}

// 清理所有显示设备的屏幕
void UserInterface::clearDisplay() {
    for (int i = 0; i < displayDeviceCount; i++) {
        displayDevices[i]->clearDisplay();
    }
}

// 获取下一个待处理的用户意图
UIIntent UserInterface::getNextIntent() {
    // 优先级1：来自触摸/终端范式的导航指令（由 dispatchPendingCommands 推入）
    if (!_intentQueue.empty()) {
        UIIntent intent = _intentQueue.front();
        _intentQueue.pop();
        return intent;
    }

    // 优先级2：旋钮范式设备（新接口，IKnobDevice）
    for (int i = 0; i < knobDeviceCount; i++) {
        if (knobDevices[i]->hasIntent()) {
            return knobDevices[i]->pollIntent();
        }
    }

    // 优先级3：旧 InputSource 数组（向下兼容，期间两套并存）
    for (int i = 0; i < inputSourceCount; i++) {
        if (inputSources[i] != nullptr) {
            inputSources[i]->tick();
        }
    }
    for (int i = 0; i < inputSourceCount; i++) {
        if (inputSources[i] != nullptr && inputSources[i]->hasIntent()) {
            return inputSources[i]->pollIntent();
        }
    }
    return UIIntent(UIAction::NONE);
}

// [已弃用] 获取编码器旋转增量
int UserInterface::getEncoderDelta() {
    return 0;
}
 
// [已弃用] 获取原始旋转增量 (1:1 高灵敏度)
int UserInterface::getRawEncoderDelta() {
    return 0;
}
 
// [已弃用] 检查主按钮是否被按下
bool UserInterface::isMasterButtonPressed() {
    return false;
}
 
// [已弃用] 检查主按钮是否被长按
bool UserInterface::isMasterButtonLongPressed() {
    return false;
}

// 启用指定输出渠道
void UserInterface::enableOutputChannel(OutputChannel channel) {
    outputChannels |= channel;
}

// 禁用指定输出渠道
void UserInterface::disableOutputChannel(OutputChannel channel) {
    outputChannels &= ~channel;
}

// 检查输出渠道是否启用
bool UserInterface::isOutputChannelEnabled(OutputChannel channel) const {
    return (outputChannels & channel) != 0;
}

// 设置所有输出渠道
void UserInterface::setOutputChannels(uint8_t channels) {
    outputChannels = channels;
}

// 获取当前输出渠道设置
uint8_t UserInterface::getOutputChannels() const {
    return outputChannels;
}

// 添加显示设备
bool UserInterface::addDisplayDevice(Display* display) {
    if (display == nullptr || displayDeviceCount >= MAX_DISPLAY_DEVICES) {
        return false;
    }
    
    // 检查设备是否已经存在
    for (int i = 0; i < displayDeviceCount; i++) {
        if (displayDevices[i] == display) {
            return false;
        }
    }
    
    // 添加新设备
    displayDevices[displayDeviceCount++] = display;
    return true;
}

// 移除显示设备
bool UserInterface::removeDisplayDevice(Display* display) {
    if (display == nullptr) {
        return false;
    }
    
    // 查找设备
    int index = -1;
    for (int i = 0; i < displayDeviceCount; i++) {
        if (displayDevices[i] == display) {
            index = i;
            break;
        }
    }
    
    if (index == -1) {
        return false;
    }
    
    // 移除设备并重新排列数组
    for (int i = index; i < displayDeviceCount - 1; i++) {
        displayDevices[i] = displayDevices[i + 1];
    }
    displayDevices[--displayDeviceCount] = nullptr;
    return true;
}

// 清除所有显示设备
void UserInterface::clearAllDisplayDevices() {
    for (int i = 0; i < displayDeviceCount; i++) {
        displayDevices[i] = nullptr;
    }
    displayDeviceCount = 0;
}

// 清除所有输入源
void UserInterface::clearAllInputSources() {
    for (int i = 0; i < inputSourceCount; i++) {
        inputSources[i] = nullptr;
    }
    inputSourceCount = 0;
}

// 设置当前语言
void UserInterface::setLanguage(Language language) {
    currentLanguage = language;
}

// 获取当前语言
Language UserInterface::getLanguage() const {
    return currentLanguage;
}

// 翻译方法（根据当前语言翻译文本）
String UserInterface::translate(const String& key) const {
    // 简单的翻译映射示例，可以扩展为更复杂的翻译系统
    if (currentLanguage == LANGUAGE_CHINESE) {
        if (key == "System ready") return "系统就绪";
        if (key == "Current Mode") return "当前模式";
        if (key == "Use mode button to switch between modes") return "使用模式按钮切换模式";
        if (key == "Mode switch requested to") return "请求切换模式到";
        if (key == "Mode switched to") return "模式已切换到";
        if (key == "Normal Mode") return "正常模式";
        if (key == "Encoder Diag") return "编码器诊断";
        if (key == "Scanner Diag") return "扫描仪诊断";
        if (key == "Outlet Diag") return "出口诊断";
        if (key == "Version Info") return "版本信息";
        if (key == "Submode") return "子模式";
        if (key == "Switch to Submode") return "切换到子模式";
        if (key == "Stats") return "统计信息";
        if (key == "Latest Diameter") return "最新直径";
        if (key == "Phase Change") return "相位变化";
        if (key == "Position") return "位置";
        if (key == "Cycle Drop (Normally Open)") return "循环降落（常开）";
        if (key == "Cycle Raise (Normally Closed)") return "循环上升（常闭）";
        if (key == "Encoder Diagnostic Mode Activated") return "编码器诊断模式已激活";
        if (key == "Use slave button to switch submode") return "使用从按钮切换子模式";
        if (key == "Outlet Diagnostic Mode Activated") return "出口诊断模式已激活";
        if (key == "Feeder Test Mode Activated") return "上料器测试模式已激活";
    }
    // 默认返回原始字符串
    return key;
}

// 输出消息到指定渠道
void UserInterface::output(const String& message, OutputChannel channels) {
    // 翻译消息
    String translatedMessage = translate(message);
    
    // 输出到串口
    if ((channels & OUTPUT_SERIAL) && isOutputChannelEnabled(OUTPUT_SERIAL)) {
        Serial.print(translatedMessage);
    }
    
    // OLED输出需要特殊处理，暂时只支持串口
    // 可以在未来扩展OLED的直接文本输出功能
}

// 输出带换行的消息到指定渠道
void UserInterface::outputLine(const String& message, OutputChannel channels) {
    String translatedMessage = translate(message);
    if ((channels & OUTPUT_SERIAL) && isOutputChannelEnabled(OUTPUT_SERIAL)) {
        Serial.println(translatedMessage);
    }
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
    // 1. 旧 Display 数组（向下兼容）
    refreshAllDevices(snapshot);

    // 2. 旋钮设备：刷新快照（在 App 页面显示数据）
    for (int i = 0; i < knobDeviceCount; i++) {
        knobDevices[i]->refreshSnapshot(snapshot);
    }

    // 3. 触摸设备：推送快照到对端
    for (int i = 0; i < touchDeviceCount; i++) {
        touchDevices[i]->pushSnapshot(snapshot);
    }

    // 4. 终端设备：ASCII 输出快照（内部自带节流）
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

            // 导航类命令 → UIIntent 队列（与旋钮范式共享处理路径）
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

            // 简单导航命令直接转 UIIntent，其余走 dispatchText
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

    // ── 3. 旋钮设备 tick（旋钮由 getNextIntent() 消费，此处只 tick）─────────
    for (int i = 0; i < knobDeviceCount; i++) {
        knobDevices[i]->tick();
    }
}

// ── getNextIntent：优先消费内部 UIIntent 队列，再轮询旋钮设备 ─────────────────
// 覆盖现有实现（此处追加版，现有的会被编译器看到两份 —— 需替换现有实现）
