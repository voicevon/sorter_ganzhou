#ifndef USER_INTERFACE_H
#define USER_INTERFACE_H

#include <Arduino.h>
#include <queue>
#include "common/display_types.h"
#include "common/ui_intent.h"
#include "paradigm/i_knob_device.h"
#include "paradigm/i_touch_device.h"
#include "paradigm/i_term_device.h"
#include "command_dispatcher.h"

class MenuNode;
enum AppType;

/**
 * @class UserInterface
 * @brief 用户界面顶层管理器（三范式架构：主要承载 RS485 TouchDevice）
 */
class UserInterface {
private:
    UserInterface();

    UserInterface(const UserInterface&) = delete;
    UserInterface& operator=(const UserInterface&) = delete;

    static UserInterface* instance;

    // ── 三范式子系统设备数组 ──────────────────────────────────────────────────
    static const int MAX_KNOB_DEVICES  = 2;
    static const int MAX_TOUCH_DEVICES = 4;
    static const int MAX_TERM_DEVICES  = 2;

    IKnobDevice*  knobDevices[MAX_KNOB_DEVICES];
    ITouchDevice* touchDevices[MAX_TOUCH_DEVICES];
    ITermDevice*  termDevices[MAX_TERM_DEVICES];
    int knobDeviceCount  = 0;
    int touchDeviceCount = 0;
    int termDeviceCount  = 0;

    // 内部意图队列（来自触摸/终端范式的导航指令）
    std::queue<UIIntent> _intentQueue;

public:
    static UserInterface* getInstance();

    void initialize();

    // ── 三范式子系统设备注册 ──────────────────────────────────────────────────
    void addKnobDevice(IKnobDevice* dev);
    void addTouchDevice(ITouchDevice* dev);
    void addTermDevice(ITermDevice* dev);

    /**
     * @brief 广播快照到所有已注册设备
     */
    void broadcastSnapshot(const DisplaySnapshot& snapshot);

    /**
     * @brief 轮询并调度所有触摸/终端设备的待处理指令
     */
    void dispatchPendingCommands();

    // 统一菜单显示代理
    void renderMenu(MenuNode* node, int cursorIndex, int scrollOffset);

    void clearDisplay();
    bool isDisplayAvailable() const;

    // 意图驱动的输入方法
    UIIntent getNextIntent();
};

#endif // USER_INTERFACE_H
