#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace IntelEngine::Persistence {
    inline constexpr uint32_t SLOT_RECORD_VERSION = 1;
    inline constexpr size_t SLOT_COUNT = 5;
    inline constexpr size_t MAX_SLOT_STRING_BYTES = 4096;
    inline constexpr size_t MAX_SLOT_RECORD_BYTES = SLOT_COUNT * (28 + 2 * MAX_SLOT_STRING_BYTES);
    struct SavedSlot {
        uint32_t formID = 0;
        int32_t state = 0;
        std::string taskType;
        std::string targetName;
        int32_t speed = 0;
        float deadline = 0;
        float offscreen = 0;
    };

    inline bool ValidSlot(const SavedSlot& value) {
        const bool state = value.state == 0 || value.state == 1 || value.state == 2 ||
            value.state == 3 || value.state == 5 || value.state == 8;
        return state && value.speed >= 0 && value.speed <= 2 &&
            std::isfinite(value.deadline) && value.deadline >= 0 &&
            std::isfinite(value.offscreen) && value.offscreen >= 0 &&
            value.taskType.size() <= MAX_SLOT_STRING_BYTES &&
            value.targetName.size() <= MAX_SLOT_STRING_BYTES &&
            (value.state == 0 || (value.formID != 0 && !value.taskType.empty()));
    }

    inline bool DecodeSlots(std::span<const unsigned char> bytes, uint32_t version,
                            std::array<SavedSlot, SLOT_COUNT>& output) {
        if (version != SLOT_RECORD_VERSION || bytes.size() < SLOT_COUNT * 28 ||
            bytes.size() > MAX_SLOT_RECORD_BYTES) return false;
        size_t offset = 0;
        auto scalar = [&]<typename T>(T& value) {
            if (sizeof(T) > bytes.size() - offset) return false;
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
            offset += sizeof(T);
            return true;
        };
        auto string = [&](std::string& value) {
            uint32_t length = 0;
            if (!scalar(length) || length > MAX_SLOT_STRING_BYTES || length > bytes.size() - offset)
                return false;
            value.assign(reinterpret_cast<const char*>(bytes.data() + offset), length);
            offset += length;
            return value.find('\0') == std::string::npos;
        };
        std::array<SavedSlot, SLOT_COUNT> decoded{};
        for (auto& slot : decoded) {
            if (!scalar(slot.formID) || !scalar(slot.state) || !string(slot.taskType) ||
                !string(slot.targetName) || !scalar(slot.speed) || !scalar(slot.deadline) ||
                !scalar(slot.offscreen) || !ValidSlot(slot)) return false;
        }
        if (offset != bytes.size()) return false;
        output = std::move(decoded);
        return true;
    }

    inline bool EncodeSlots(const std::array<SavedSlot, SLOT_COUNT>& slots,
                            std::vector<unsigned char>& output) {
        std::vector<unsigned char> bytes;
        auto scalar = [&]<typename T>(const T& value) {
            const auto* first = reinterpret_cast<const unsigned char*>(&value);
            bytes.insert(bytes.end(), first, first + sizeof(T));
        };
        auto string = [&](const std::string& value) {
            scalar(static_cast<uint32_t>(value.size()));
            bytes.insert(bytes.end(), value.begin(), value.end());
        };
        for (const auto& slot : slots) {
            if (!ValidSlot(slot)) return false;
            scalar(slot.formID); scalar(slot.state);
            string(slot.taskType); string(slot.targetName);
            scalar(slot.speed); scalar(slot.deadline); scalar(slot.offscreen);
        }
        output = std::move(bytes);
        return true;
    }
}
