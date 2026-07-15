#ifndef I_KNOB_DEVICE_H
#define I_KNOB_DEVICE_H

#include <Arduino.h>
#include "../common/ui_intent.h"
#include "../common/display_types.h"

class MenuNode;

/**
 * @interface IKnobDevice
 * @brief 旋钮菜单范式设备接口
 *
 * 代表一个"旋钮菜单 HMI 单元"，同时承担：
 *   - 输入：产生 UIIntent（NAVIGATE_RELATIVE / ACTIVATE / BACK / CANCEL）
 *   - 输出：渲染菜单树节点到本地显示器，可选显示数据快照
 *
 * 典型实现：OledKnobDevice（OLED + 旋转编码器 的 Façade）
 */
class IKnobDevice {
public:
    virtual ~IKnobDevice() = default;

    /**
     * @brief 初始化硬件
     */
    virtual void initialize() = 0;

    /**
     * @brief 心跳驱动（在 UI 任务主循环中调用）
     * 用于轮询、消抖或收割中断标志。
     */
    virtual void tick() = 0;

    /**
     * @brief 是否有待处理的意图（非破坏性检查）
     */
    virtual bool hasIntent() = 0;

    /**
     * @brief 消费并返回当前意图（只产生导航类意图）
     * @return UIIntent，若无意图则返回 NONE
     */
    virtual UIIntent pollIntent() = 0;

    /**
     * @brief 渲染菜单树节点到本地显示器
     * @param node        当前菜单节点
     * @param cursor      游标位置（节点内相对索引）
     * @param scroll      滚动偏移
     */
    virtual void renderMenu(MenuNode* node, int cursor, int scroll) = 0;

    /**
     * @brief 刷新数据快照（在 App 叶节点页面显示数据，可选实现）
     * 默认空实现，旋钮设备如有屏幕可选择渲染快照数据。
     */
    virtual void refreshSnapshot(const DisplaySnapshot& snapshot) {}

    /**
     * @brief 清除显示
     */
    virtual void clearDisplay() {}

    /**
     * @brief 设备是否可用（输出侧，如 OLED 可能 Init Failed）
     */
    virtual bool isAvailable() const = 0;

    /**
     * @brief 设备名称（调试用）
     */
    virtual String getName() const = 0;
};

#endif // I_KNOB_DEVICE_H
