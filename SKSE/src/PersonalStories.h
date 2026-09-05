#pragma once

#include "Plugin.h"
#include "PersonalStoryState.h"

namespace IntelEngine::PersonalStories {
    inline constexpr std::uint32_t kRecordType = RECORD_TYPE;
    inline constexpr std::uint32_t kRecordVersion = RECORD_VERSION;

    // All lifecycle and Tick calls run on Skyrim's main thread. SkyrimNet
    // callbacks only copy values or read the published actor context cache.
    void Initialize();
    void Tick();
    void ResumeSession();
    void Revert();
    void Save(SKSE::SerializationInterface* serialization);
    void Load(SKSE::SerializationInterface* serialization, std::uint32_t version, std::uint32_t length);
}
