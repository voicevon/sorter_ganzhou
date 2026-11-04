#include "app_encoder_diag.h"
#include "../modular/encoder.h"
#include "../user_interface/common/display_types.h"


AppEncoderDiag::AppEncoderDiag() {
    encoder = Encoder::getInstance();
    currentSubMode = 0;
    subModeInitialized = false;
    userInterface = nullptr;
}

void AppEncoderDiag::begin() {
    Serial.println("[DIAGNOSTIC] Encoder Diagnostic Started");
    subModeInitialized = false;
    lastUIDisplayTime = 0;
}

void AppEncoderDiag::end() {
    Serial.println("[DIAGNOSTIC] Encoder Diagnostic Ended");
}

void AppEncoderDiag::initialize(UserInterface* ui) {
    userInterface = ui;
}

void AppEncoderDiag::update(uint32_t currentMs, bool btnPressed) {
    (void)btnPressed;
    if (currentMs - lastUIDisplayTime >= UI_REFRESH_INTERVAL) {
        lastUIDisplayTime = currentMs;
    }
}

void AppEncoderDiag::switchToNextSubMode() {
    currentSubMode = (currentSubMode + 1) % 3;
    Serial.println("[DIAGNOSTIC] Encoder Submode: " + String(currentSubMode));
}

void AppEncoderDiag::captureSnapshot(DisplaySnapshot& snapshot) {
    snapshot.currentMode = APP_DIAG_ENCODER;
    strcpy(snapshot.activePage, "diag_encoder");
    
    snapshot.data.encoder.raw = encoder->getRawCount();
    snapshot.data.encoder.corrected = encoder->getZeroCrossRawCount();
    snapshot.data.encoder.logic = encoder->getCurrentPosition();
    snapshot.data.encoder.zeroCount = encoder->getZeroCrossCount() - encoder->getForcedZeroCount(); // correctZ
    snapshot.data.encoder.zeroCorrect = encoder->getZeroCrossCount(); // totalZ
    snapshot.data.encoder.zeroTotal = encoder->getForcedZeroCount(); // errorZ
    snapshot.data.encoder.offset = encoder->getPhaseOffset();
}
