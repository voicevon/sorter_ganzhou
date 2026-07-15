#ifndef DIAMETER_SCANNER_H
#define DIAMETER_SCANNER_H

#ifndef TOTAL_SCAN_CHANNELS
#define TOTAL_SCAN_CHANNELS 4
#endif

#include <Arduino.h>
#include "../config.h"
#include <atomic>
#include "soc/gpio_struct.h" // 增加对 GPIO 寄存器结构的引用

#include "../utils/singleton.h"

class DiameterScanner : public Singleton<DiameterScanner> {
    friend class Singleton<DiameterScanner>;
private:
    int scannerPins[TOTAL_SCAN_CHANNELS];
    
    // 预计算的快速 GPIO 寄存器读取结构
    struct FastPin {
        volatile uint32_t* reg;
        uint32_t mask;
    };
    FastPin fastPins[TOTAL_SCAN_CHANNELS];

    std::atomic<bool> isScanning;
    // 高电平采样计数（每个扫描点）
    volatile int highLevelPulseCounts[TOTAL_SCAN_CHANNELS];
    
    // 统计次数（计算实际物体数量）- 每个扫描点一个计数器
    volatile int objectCount[TOTAL_SCAN_CHANNELS];
    
    // 上一次的传感器状态（每个扫描点）
    bool previousSensorStates[TOTAL_SCAN_CHANNELS];
    volatile int previousSampledPhase; // 记录上一次处理的相位 (ISR 内部使用)
    volatile bool isObjectDetected[TOTAL_SCAN_CHANNELS];

    static const int MAX_SAMPLES = 200;
    // 状态压缩：每个 uint8_t 存放同一次采样的多个通道状态（位存储），大幅降低内存占用和 ISR 内存写入开销
    volatile uint8_t sensorBuffers[MAX_SAMPLES];
    std::atomic<int> sampleCount;

    // 计算得到的直径值（整数）
    int nominalDiameter;
    

    // 私有构造函数，防止外部创建实例
    DiameterScanner();

public:
    // 单例模式获取实例的静态方法 - Managed by Singleton
    // static DiameterScanner* getInstance();
    
    // 初始化引脚和缓冲区
    void initialize();
    
    // 重置状态和缓冲区
    void start();
    
    // 停止扫描计数
    // 停止扫描计数
    void IRAM_ATTR stop();

    // 检查是否正在扫描
    bool isScanningActive() const { return isScanning; }
    
    // 采样传感器状态（根据相位进行采样）- 用于直径测量
    void IRAM_ATTR sample(int phase);
    
    // 获取计算的直径值（整数）并停止扫描 (Calculation moved here)
    int getDiameterAndStop();
    
    // 获取统计的物体数量
    int getObjectCount(int index) const;
    
    // 获取长度级别 (1:S, 2:M, 3:L)
    int getLengthLevel();
    
    // 获取所有扫描点的物体数量总和
    int getTotalObjectCount() const;

    // 获取缓冲区的总采样数
    int getSampleCount() const { return sampleCount; }
    
    // 获取特定扫描点在特定采样阶段的状态
    uint8_t getSample(int sensorIndex, int sampleIndex) const {
        if (sensorIndex >= 0 && sensorIndex < TOTAL_SCAN_CHANNELS && sampleIndex >= 0 && sampleIndex < sampleCount) {
            return (sensorBuffers[sampleIndex] & (1 << sensorIndex)) ? 1 : 0;
        }
        return 0;
    }

    // 获取IO状态数组（用于诊断模式子模式1）
    bool* getIOStatusArray();

    // 获取单个传感器的高电平脉冲计数
    int getHighLevelPulseCount(int index) const;
    
    // 获取传感器权重
    float getSensorWeight(int index) const;
};

#endif // DIAMETER_SCANNER_H