#ifndef COMMAND_DISPATCHER_H
#define COMMAND_DISPATCHER_H

#include <Arduino.h>
#include <functional>
#include <map>
#include "paradigm/i_touch_device.h"  // RichCommand
#include "common/ui_intent.h"

/**
 * @class CommandDispatcher
 * @brief 富指令调度器
 *
 * 解耦触摸范式驱动层与业务层的核心机制：
 *
 *   触摸/终端设备 → 产生 RichCommand / 文本行
 *       ↓
 *   CommandDispatcher::dispatch()
 *       ↓
 *   已注册的业务 Handler（lambda / 函数指针）
 *       ↓
 *   sorter / encoder / appOutletDiag 等业务对象
 *
 * 业务层在 main.cpp 的 setupCommandDispatcher() 中注册处理器，
 * 驱动层完全不需要 #include 任何业务头文件。
 *
 * 使用方式：
 *   // 注册
 *   CommandDispatcher::getInstance().registerHandler("set_offset",
 *       [](const RichCommand& c) {
 *           encoder->setPhaseOffset(c.paramI);
 *       });
 *
 *   // 分发（由 UserInterface 在每帧调用）
 *   CommandDispatcher::getInstance().dispatch(cmd);
 *
 *   // 文本行分发（由 TermSubsystem 调用）
 *   CommandDispatcher::getInstance().dispatchText("set_offset:12");
 */
class CommandDispatcher {
public:
    using Handler = std::function<void(const RichCommand&)>;

    /**
     * @brief 获取单例实例
     */
    static CommandDispatcher& getInstance() {
        static CommandDispatcher instance;
        return instance;
    }

    // 禁止拷贝
    CommandDispatcher(const CommandDispatcher&) = delete;
    CommandDispatcher& operator=(const CommandDispatcher&) = delete;

    /**
     * @brief 注册命令处理器
     * @param cmd     命令标识符（与 RichCommand::cmd 匹配）
     * @param handler 处理函数（lambda 或函数指针）
     *
     * 若同一 cmd 多次注册，后注册的 handler 覆盖前一个。
     */
    void registerHandler(const String& cmd, Handler handler) {
        _handlers[cmd] = handler;
    }

    /**
     * @brief 分发一条富指令
     * @param cmd 要分发的 RichCommand
     *
     * 若无对应 handler，输出 Warning 日志后静默忽略。
     */
    void dispatch(const RichCommand& cmd) {
        if (!cmd.isValid()) return;

        auto it = _handlers.find(cmd.cmd);
        if (it != _handlers.end()) {
            it->second(cmd);
        } else {
            Serial.printf("[CommandDispatcher] WARNING: No handler for cmd='%s'\n",
                          cmd.cmd.c_str());
        }
    }

    /**
     * @brief 将文本命令行解析为 RichCommand 后分发
     * @param line 已 trim 的文本行，如 "set_offset:12" / "ACTIVATE" / "NAV:+1"
     *
     * 解析规则（按优先级）：
     *   1. "CMD:VALUE" → cmd=CMD, paramI=VALUE（整型）/ strParam=VALUE（字符串）
     *   2. "CMD"       → cmd=CMD, 无参数
     *
     * 注：UIAction 类导航命令（ACTIVATE/BACK/NAV等）需要业务层自行注册 handler，
     *     或由 UserInterface 在调用前先判断并转换为 UIIntent。
     */
    void dispatchText(const String& line) {
        if (line.length() == 0) return;
        RichCommand cmd = parseText(line);
        dispatch(cmd);
    }

    /**
     * @brief 检查是否有指定命令的 handler
     */
    bool hasHandler(const String& cmd) const {
        return _handlers.find(cmd) != _handlers.end();
    }

    /**
     * @brief 清除所有已注册的 handler（测试用）
     */
    void clearHandlers() {
        _handlers.clear();
    }

private:
    CommandDispatcher() = default;

    std::map<String, Handler> _handlers;

    /**
     * @brief 内部：将文本行解析为 RichCommand
     *
     * 格式："CMD" 或 "CMD:VALUE"
     * VALUE 可以是整数（paramI）、浮点数（paramF）或字符串（strParam）。
     * 解析策略：先尝试整数，再尝试浮点，最后作为字符串。
     */
    RichCommand parseText(const String& line) {
        RichCommand cmd;

        int colonIdx = line.indexOf(':');
        if (colonIdx == -1) {
            // 无参数命令，如 "ACTIVATE" / "BACK" / "save_config"
            cmd.cmd = line;
            cmd.cmd.trim();
        } else {
            cmd.cmd = line.substring(0, colonIdx);
            cmd.cmd.trim();

            String valueStr = line.substring(colonIdx + 1);
            valueStr.trim();
            cmd.strParam = valueStr;

            // 尝试解析为整数
            bool isNumeric = true;
            int startIdx = 0;
            if (valueStr.length() > 0 && (valueStr[0] == '+' || valueStr[0] == '-')) {
                startIdx = 1;
            }
            for (int i = startIdx; i < (int)valueStr.length(); i++) {
                if (!isDigit(valueStr[i])) {
                    isNumeric = false;
                    break;
                }
            }

            if (isNumeric && valueStr.length() > 0) {
                cmd.paramI = valueStr.toInt();
                cmd.paramF = (float)cmd.paramI;
            } else {
                // 尝试浮点
                bool isFloat = true;
                bool hasDot = false;
                for (int i = startIdx; i < (int)valueStr.length(); i++) {
                    if (valueStr[i] == '.' && !hasDot) { hasDot = true; continue; }
                    if (!isDigit(valueStr[i])) { isFloat = false; break; }
                }
                if (isFloat && valueStr.length() > 0) {
                    cmd.paramF = valueStr.toFloat();
                    cmd.paramI = (int)cmd.paramF;
                }
                // 否则 strParam 已赋值，保持字符串形式
            }
        }

        return cmd;
    }
};

#endif // COMMAND_DISPATCHER_H
