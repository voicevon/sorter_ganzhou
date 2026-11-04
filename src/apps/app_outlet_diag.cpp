#include "app_outlet_diag.h"
#include "config.h"
#include "../user_interface/common/display_types.h"


AppOutletDiag::AppOutletDiag() : 
    modeStartTime(0), 
    lastOutletTime(0), 
    outletState(false), 
    currentOutlet(0), 
    displayInitialized(false), 
    currentSubMode(3), // 默认进入手动触控测试模式（RS485 触摸屏）
    lastSubMode(-1),
    lastUpdateTime(0),
    cycleCount(0), 
    userInterface(nullptr) {
    for (int i = 0; i < NUM_OUTLETS; i++) {
        outlets[i] = nullptr;
    }
}

void AppOutletDiag::begin() {
    Serial.println("[DIAGNOSTIC] Outlet Diagnostic Started");
    modeStartTime = 0; // Trigger initialization in update()
}

void AppOutletDiag::end() {
    Serial.println("[DIAGNOSTIC] Outlet Diagnostic Ended");
    // Ensure all outlets are closed on exit
    for (int i = 0; i < NUM_OUTLETS; i++) {
        if (outlets[i]) {
            outlets[i]->setReadyToOpen(false);
            outlets[i]->execute();
        }
    }
}

void AppOutletDiag::initialize(UserInterface* ui) {
    userInterface = ui;
}

void AppOutletDiag::setOutlet(uint8_t index, Outlet* outlet) {
    if (index < NUM_OUTLETS) {
        outlets[index] = outlet;
    }
}

void AppOutletDiag::setSubMode(int mode) {
    currentSubMode = mode;
    lastSubMode = -1; // 强制触发一次显示刷新
    cycleCount = 0;
    outletState = false;
    
    String subModeName = "";
    switch(mode) {
        case 0: subModeName = "Normally Closed Test"; break;
        case 1: subModeName = "Single Outlet Test"; break;
        case 2: subModeName = "Lifetime Cycle Test"; break;
        case 3: subModeName = "Manual Touch Test"; break;
        default: subModeName = "Unknown Mode"; break;
    }
    Serial.println("[DIAGNOSTIC] Outlet Mode explicitly set to: " + subModeName);
}

void AppOutletDiag::update(uint32_t currentMs, bool btnPressed) {
    (void)btnPressed;
    
    // 模式开始时初始化
    if (modeStartTime == 0 || currentSubMode != lastSubMode) {
        initializeDiagnosticMode(currentMs);
        lastSubMode = currentSubMode;
    }
    
    uint32_t interval;
    String testType;
    
    switch (currentSubMode) {
        case 0: {
            interval = outletState ? 1000 : 500;
            testType = "normally closed";
            processCycleOperation(currentMs, interval, testType);
            break;
        }
        case 1: {
            interval = outletState ? 300 : 500;
            if (currentMs - lastOutletTime >= interval) {
                lastOutletTime = currentMs;
                outletState = !outletState;
                
                for (int i = 0; i < NUM_OUTLETS; i++) {
                    if (i == currentOutlet) {
                        outlets[i]->setReadyToOpen(outletState);
                    } else {
                        outlets[i]->setReadyToOpen(false);
                    }
                    outlets[i]->execute();
                }
            }
            break;
        }
        case 2: {
            interval = outletState ? 300 : 100;
            testType = "lifetime test";  
            
            if (currentMs - lastOutletTime >= interval) {
                lastOutletTime = currentMs;
                outletState = !outletState;
                
                if (outletState) {
                    cycleCount++;
                } else {
                    currentOutlet = (currentOutlet + 1) % NUM_OUTLETS;
                }
                
                for (int i = 0; i < NUM_OUTLETS; i++) {
                    if (i == currentOutlet) {
                        outlets[i]->setReadyToOpen(outletState);
                    } else {
                        outlets[i]->setReadyToOpen(false);
                    }
                    outlets[i]->execute();
                }
            }
            break;
        }
        case 3: {
            // 子模式3：手动触控测试模式 (Manual Touch Test)
            // 通过 RS485 触摸屏手动点击独立控制，不自动循环
            break;
        }
    }
}

void AppOutletDiag::processCycleOperation(uint32_t currentTime, uint32_t interval, const String& testType) {
    if (currentTime - lastOutletTime >= interval) {
        lastOutletTime = currentTime;
        outletState = !outletState;
        
        if (outletState) {
            currentOutlet = (currentOutlet + 1) % NUM_OUTLETS;
            Serial.print("[DIAGNOSTIC] Now testing outlet " + testType + ": ");
            Serial.println(currentOutlet);
        }
        
        outlets[currentOutlet]->setReadyToOpen(outletState);
        outlets[currentOutlet]->execute();
    }
}

void AppOutletDiag::initializeDiagnosticMode(uint32_t currentTime) {
    modeStartTime = currentTime;
    lastOutletTime = currentTime;
    outletState = false;
    currentOutlet = 0;
    displayInitialized = false;
    cycleCount = 0;
    
    if (currentSubMode != 2 && currentSubMode != 3) {
        outlets[currentOutlet]->setReadyToOpen(outletState);
        outlets[currentOutlet]->execute();
        
        Serial.print("[DIAGNOSTIC] Initial outlet state normally closed: ");
        Serial.print(currentOutlet);
        Serial.print(" - ");
        Serial.println(outletState ? "Open" : "Closed");
    }
    
    String subModeName;
    switch (currentSubMode) {
        case 0:
            subModeName = "Cycle Drop (Normally Closed)";
            break;
        case 1:
            subModeName = "Single Outlet Test";
            break;
        case 2:
            subModeName = "Solenoid Lifetime Test";
            Serial.println("\n=== Solenoid Lifetime Test ===");
            Serial.println("Cycle: 0 | State: Closed");
            break;
        case 3:
            subModeName = "Manual Touch Test";
            break;
        default:
            subModeName = "Unknown";
    }
    if (currentSubMode != 2) {
        Serial.println("[DIAGNOSTIC] Outlet Diagnostic Mode Activated - Submode: " + subModeName);
    }
}

void AppOutletDiag::handleEncoderInput(int delta) {
    if (currentSubMode == 1 && delta != 0) {
        int newOutlet = (int)currentOutlet + delta;
        while (newOutlet < 0) {
            newOutlet += NUM_OUTLETS;
        }
        currentOutlet = (uint8_t)(newOutlet % NUM_OUTLETS);
        
        outletState = false;
        lastOutletTime = millis();
        
        for (int i = 0; i < NUM_OUTLETS; i++) {
            outlets[i]->setReadyToOpen(false);
            outlets[i]->execute();
        }
        
        Serial.print("[DIAGNOSTIC] Selected outlet changed to: ");
        Serial.println(currentOutlet);
    }
}

void AppOutletDiag::setManualOutletState(uint8_t index, bool state) {
    if (currentSubMode != 3) {
        setSubMode(3); // 自动切换到手动模式
    }
    if (index < NUM_OUTLETS && outlets[index]) {
        outlets[index]->setReadyToOpen(state);
        outlets[index]->execute();
        Serial.printf("[DIAGNOSTIC] Manual Touch: Outlet %d -> %s\n", index, state ? "OPEN" : "CLOSED");
    }
}

void AppOutletDiag::captureSnapshot(DisplaySnapshot& snapshot) {
    snapshot.currentMode = APP_DIAG_OUTLET;
    strcpy(snapshot.activePage, "diag_outlets");
    
    for (int i = 0; i < 8; i++) {
        if (i < NUM_OUTLETS && outlets[i]) {
            snapshot.data.outlet.outlets[i].isOpen = outlets[i]->isPositionOpen();
            snapshot.data.outlet.outlets[i].min = outlets[i]->getMatchDiameterMin();
            snapshot.data.outlet.outlets[i].max = outlets[i]->getMatchDiameterMax();
            snapshot.data.outlet.outlets[i].mask = outlets[i]->getTargetLength();
        } else {
            snapshot.data.outlet.outlets[i].isOpen = false;
            snapshot.data.outlet.outlets[i].min = 0;
            snapshot.data.outlet.outlets[i].max = 0;
            snapshot.data.outlet.outlets[i].mask = 0;
        }
    }
    snapshot.data.outlet.activeOutletIndex = currentOutlet;
    snapshot.data.outlet.subMode = currentSubMode;
    snapshot.data.outlet.cycleCount = cycleCount;
}
