#pragma once

#include <cstdint>
#include "DisplayMessage.h"

struct DoorPopupDebugLogEntry
{
    bool valid = false;
    uint32_t sequence = 0;
    uint64_t timestampMs = 0;
    uint8_t presetIndex = 0;
    bool showPhase = false;
    DisplayMessageStruct displayMessage{};
};

struct DoorPopupDebugState
{
    static constexpr uint8_t LogCapacity = 32;

    bool enabled = false;
    bool finished = false;
    bool showPhase = false;
    bool overrideActive = false;

    uint8_t currentPresetIndex = 0;
    uint8_t presetCount = 0;
    uint8_t currentDoorStatus1 = 0;
    uint8_t currentDoorStatus2 = 0;
    uint32_t sequence = 0;
    uint8_t logWriteIndex = 0;
    uint8_t logCount = 0;

    uint64_t startedAtMs = 0;
    uint64_t phaseStartedAtMs = 0;
    uint64_t lastTransitionAtMs = 0;
    uint64_t lastFramePreparedAtMs = 0;
    uint64_t overrideReleaseAtMs = 0;

    char currentStateKey[48]{};
    char currentStateLabel[96]{};
    DisplayMessageStruct overrideDisplayMessage{};
    DoorPopupDebugLogEntry logEntries[LogCapacity]{};

    void Reset()
    {
        *this = DoorPopupDebugState{};
    }
};
