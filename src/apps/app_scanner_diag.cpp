#include "app_scanner_diag.h"
#include "../user_interface/common/display_types.h"


AppScannerDiag::AppScannerDiag() : 
    userInterface(UserInterface::getInstance()),
    currentSubMode(0),
    lastSubMode(-1),
    hasCalculatedDifferences(false),
    isFirstRun(true) {
    // 获取直径扫描仪单例实例
    scanner = DiameterScanner::getInstance();
    encoder = Encoder::getInstance();
    
    // 初始化编码器值数组
    for (int i = 0; i < 4; i++) {
        risingEdgeEncoderValues[i] = 0;
        fallingEdgeEncoderValues[i] = 0;
        minRisingEdgeValues[i] = 200; // 初始化为最大值，确保第一个值会被记录为最小值
        maxFallingEdgeValues[i] = 0;  // 初始化为最小值，确保第一个值会被记录为最大值
        diameterDifferences[i] = 0;
        lastSensorStates[i] = false;
        risingEdges[i] = false;
        fallingEdges[i] = false;
        lastRawDiameters[i] = -1; // 初始化为不可能的值，确保首次执行时会输出
        risingEdgeCounts[i] = 0; // 初始化上升沿计数器为0
    }
    
    lastIOStatus = "";
}

void AppScannerDiag::displayRawDiameters() {
    bool isFirst = true;
    for (int i = 0; i < 4; i++) {
        if (lastRawDiameters[i] != -1) {
            isFirst = false;
            break;
        }
    }

    if (isFirst) {
        Serial.println("\n\033[44m\033[31m      === Raw Diameter Values ===      \033[0m");
        Serial.println("\033[44m\033[37m                                      \033[0m");
    } else {
        Serial.print("\033[6A");
        Serial.println("\033[44m\033[31m      === Raw Diameter Values ===      \033[0m");
        Serial.println("\033[44m\033[37m                                      \033[0m");
    }

    for (int i = 0; i < 4; i++) {
        int count = scanner->getHighLevelPulseCount(i);
        float weight = scanner->getSensorWeight(i);
        float corrected = count * weight;
        lastRawDiameters[i] = count;

        String line = "  Scanner " + String(i + 1) + ": " + String(count) + " (corrected: " + String(corrected, 1) + ")";
        while (line.length() < 40) {
            line += " ";
        }
        Serial.println("\033[44m\033[37m" + line + "\033[0m");
    }
}

void AppScannerDiag::handleIOStatusCheck() {
    bool* ioStates = scanner->getIOStatusArray();
    
    bool stateChanged = false;
    for (int i = 0; i < 4; i++) {
        if (ioStates[i] && !lastSensorStates[i]) {
            risingEdgeCounts[i]++;
            stateChanged = true;
        }
        lastSensorStates[i] = ioStates[i];
    }
    
    String statusLine = "IO: ";
    for (int i = 0; i < 4; i++) {
        statusLine += String(ioStates[i] ? "HI" : "LO");
        if (i < 3) {
            statusLine += " ";
        }
    }
    
    String countLine = "CNT:";
    for (int i = 0; i < 4; i++) {
        String countStr = String(risingEdgeCounts[i]);
        while (countStr.length() < 3) countStr = "0" + countStr;
        countLine += countStr;
        if (i < 3) {
            countLine += " ";
        }
    }
    
    String combinedStatus = statusLine + " | " + countLine;
    
    if (combinedStatus != lastIOStatus || stateChanged) {
        if (lastIOStatus == "") {
            Serial.println("\n\033[44m\033[31m        === Scanner IO Status ===        \033[0m");
            Serial.println("\033[44m\033[37m" + statusLine + "                      \033[0m");
            Serial.println("\033[44m\033[37m" + countLine + "                      \033[0m");
        } else {
            Serial.print("\033[3A");
            Serial.println("\033[44m\033[31m        === Scanner IO Status ===        \033[0m");
            Serial.println("\033[44m\033[37m" + statusLine + "                      \033[0m");
            Serial.println("\033[44m\033[37m" + countLine + "                      \033[0m");
        }
        lastIOStatus = combinedStatus;
    }
}

void AppScannerDiag::handleEncoderValues() {
    bool* currentStates = scanner->getIOStatusArray();
    bool hasEdgeChanged = false;
    
    for (int i = 0; i < 4; i++) {
        if (currentStates[i] && !lastSensorStates[i]) {
            risingEdges[i] = true;
            risingEdgeEncoderValues[i] = encoder->getRawCount() % 200;
            if (risingEdgeEncoderValues[i] < 0) {
                risingEdgeEncoderValues[i] += 200;
            }
            if (risingEdgeEncoderValues[i] < minRisingEdgeValues[i]) {
                minRisingEdgeValues[i] = risingEdgeEncoderValues[i];
            }
            hasEdgeChanged = true;
        } else if (!currentStates[i] && lastSensorStates[i]) {
            fallingEdges[i] = true;
            fallingEdgeEncoderValues[i] = encoder->getRawCount() % 200;
            if (fallingEdgeEncoderValues[i] < 0) {
                fallingEdgeEncoderValues[i] += 200;
            }
            if (fallingEdgeEncoderValues[i] > maxFallingEdgeValues[i]) {
                maxFallingEdgeValues[i] = fallingEdgeEncoderValues[i];
            }
            int diff = fallingEdgeEncoderValues[i] - risingEdgeEncoderValues[i];
            if (diff < 0) {
                diff += 200;
            }
            diameterDifferences[i] = diff;
            hasCalculatedDifferences = true;
            hasEdgeChanged = true;
        }
        lastSensorStates[i] = currentStates[i];
    }
    
    if (hasEdgeChanged) {
        if (isFirstRun) {
            Serial.println("\n\033[44m\033[31m    === Encoder Value Recording ===    \033[0m");
            Serial.println("\033[44m\033[37m  Min Rising:      0   0   0   0      \033[0m");
            Serial.println("\033[44m\033[37m  Current Rising:  0   0   0   0      \033[0m");
            Serial.println("\033[44m\033[37m  Current Falling: 0   0   0   0      \033[0m");
            Serial.println("\033[44m\033[37m  Max Falling:     0   0   0   0      \033[0m");
            Serial.println("\033[44m\033[37m  Differences:     0   0   0   0      \033[0m");
            isFirstRun = false;
        } else {
            Serial.print("\033[6A");
            Serial.print("\033[44m\033[31m    === Encoder Value Recording ===    \033[0m"); Serial.println();
            
            Serial.print("\033[44m\033[37m  Min Rising:");
            for (int i = 0; i < 4; i++) {
                Serial.print(" ");
                if (minRisingEdgeValues[i] < 10) Serial.print("  ");
                else if (minRisingEdgeValues[i] < 100) Serial.print(" ");
                Serial.print(minRisingEdgeValues[i]);
            }
            Serial.println("      \033[0m");
            
            Serial.print("\033[44m\033[37m  Current Rising:");
            for (int i = 0; i < 4; i++) {
                Serial.print(" ");
                if (risingEdgeEncoderValues[i] < 10) Serial.print("  ");
                else if (risingEdgeEncoderValues[i] < 100) Serial.print(" ");
                Serial.print(risingEdgeEncoderValues[i]);
            }
            Serial.println("      \033[0m");
            
            Serial.print("\033[44m\033[37m  Current Falling:");
            for (int i = 0; i < 4; i++) {
                Serial.print(" ");
                if (fallingEdgeEncoderValues[i] < 10) Serial.print("  ");
                else if (fallingEdgeEncoderValues[i] < 100) Serial.print(" ");
                Serial.print(fallingEdgeEncoderValues[i]);
            }
            Serial.println("      \033[0m");
            
            Serial.print("\033[44m\033[37m  Max Falling:");
            for (int i = 0; i < 4; i++) {
                Serial.print(" ");
                if (maxFallingEdgeValues[i] < 10) Serial.print("  ");
                else if (maxFallingEdgeValues[i] < 100) Serial.print(" ");
                Serial.print(maxFallingEdgeValues[i]);
            }
            Serial.println("      \033[0m");
            
            if (hasCalculatedDifferences) {
                Serial.print("\033[44m\033[37m  Differences:");
                for (int i = 0; i < 4; i++) {
                    Serial.print(" ");
                    if (diameterDifferences[i] < 10) Serial.print("  ");
                    else if (diameterDifferences[i] < 100) Serial.print(" ");
                    Serial.print(diameterDifferences[i]);
                }
                Serial.println("      \033[0m");
            } else {
                Serial.println("\033[44m\033[37m  Differences:     0   0   0   0      \033[0m");
            }
        }
    }
}

void AppScannerDiag::handleRawDiameterDisplay() {
    bool diametersChanged = false;
    int currentDiameters[4];
    
    for (int i = 0; i < 4; i++) {
        currentDiameters[i] = scanner->getHighLevelPulseCount(i);
        if (currentDiameters[i] != lastRawDiameters[i]) {
            diametersChanged = true;
            lastRawDiameters[i] = currentDiameters[i];
        }
    }
    
    if (diametersChanged) {
        displayRawDiameters();
    }
}

void AppScannerDiag::handleWaveformDisplay() {
    // 波形显示由 snapshot 统一托管
}

void AppScannerDiag::begin() {
    lastIOStatus = "";
    isFirstRun = true;
    Serial.printf("[DIAGNOSTIC] Scanner Diagnostic Started (Mode: %d)\n", currentSubMode);
    
    switch (currentSubMode) {
        case 0: handleIOStatusCheck(); break;
        case 1: handleEncoderValues(); break;
        case 2: handleWaveformDisplay(); break;
    }
}

void AppScannerDiag::update(uint32_t currentTime, bool btnPressed) {
    (void)currentTime;
    (void)btnPressed;
    
    switch (currentSubMode) {
        case 0:
            handleIOStatusCheck();
            break;
        case 1:
            handleEncoderValues();
            break;
        case 2:
            handleWaveformDisplay();
            break;
        case 3:
            break;
    }
}

void AppScannerDiag::switchToNextSubMode() {
    currentSubMode = (currentSubMode + 1) % 4;
    if (currentSubMode == 0) {
        for (int i = 0; i < 4; i++) {
            risingEdgeCounts[i] = 0;
        }
        lastIOStatus = "";
    }
}

void AppScannerDiag::setSubMode(int mode) {
    if (mode >= 0 && mode < 4) {
        currentSubMode = mode;
        if (currentSubMode == 0) {
            for (int i = 0; i < 4; i++) {
                risingEdgeCounts[i] = 0;
            }
        }
        lastIOStatus = "";
        for (int i = 0; i < 4; i++) lastRawDiameters[i] = -1;
    }
}

int AppScannerDiag::getCurrentSubMode() const {
    return currentSubMode;
}

void AppScannerDiag::captureSnapshot(DisplaySnapshot& snapshot) {
    snapshot.currentMode = APP_DIAG_SCANNER;
    strcpy(snapshot.activePage, "diag_laser");
    
    bool* ioStates = scanner->getIOStatusArray();
    uint8_t states = 0;
    for (int i = 0; i < TOTAL_SCAN_CHANNELS; i++) {
        if (ioStates[i]) {
            states |= (1 << i);
        }
    }
    snapshot.data.scanner.states = states;
    snapshot.data.scanner.sampleCount = scanner->getSampleCount();
    
    for (int i = 0; i < TOTAL_SCAN_CHANNELS; i++) {
        for (int j = 0; j < SCAN_BUFFER_BYTES; j++) {
            uint8_t byteVal = 0;
            for (int bit = 0; bit < 8; bit++) {
                int sampleIdx = j * 8 + bit;
                if (sampleIdx < snapshot.data.scanner.sampleCount && scanner->getSample(i, sampleIdx)) {
                    byteVal |= (1 << (7 - bit));
                }
            }
            snapshot.data.scanner.history[i][j] = byteVal;
        }
    }
}
