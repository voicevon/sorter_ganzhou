#include "app_about.h"
#include <Arduino.h>
#include "../main.h"
#include "../user_interface/common/display_types.h"

AppAbout::AppAbout() {}

void AppAbout::begin() {
    Serial.println("[AppAbout] About App Started");
}

void AppAbout::update(uint32_t currentTime, bool btnPressed) {
    (void)currentTime;
    (void)btnPressed;
}

void AppAbout::end() {
    Serial.println("[AppAbout] About App Stopped");
}

void AppAbout::captureSnapshot(DisplaySnapshot& snapshot) {
    snapshot.currentMode = APP_VERSION_INFO;
    strcpy(snapshot.activePage, "About");
}
