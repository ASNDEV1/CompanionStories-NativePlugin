/**
 * Slot Tracker Implementation
 *
 * C++ mirror of Papyrus slot state for synchronous SkyrimNet decorator access.
 * Papyrus pushes state changes via native functions; C++ reads from here.
 */

#include "SlotTracker.h"
#include "SlotRecord.h"
#include "SkyrimNetAPI.h"
#include <chrono>
#include <nlohmann/json.hpp>

namespace IntelEngine {

    // Helper: get monotonic real-time seconds (survives game pause)
    static float GetRealTimeSeconds() {
        using clock = std::chrono::steady_clock;
        static auto start = clock::now();
        auto now = clock::now();
        return std::chrono::duration<float>(now - start).count();
    }

    void SlotTracker::UpdateSlot(int slot, RE::Actor* agent, int state,
                                  const std::string& taskType, const std::string& targetName) {
        if (slot < 0 || slot >= MAX_SLOTS) return;

        RE::FormID newFormId = agent ? agent->GetFormID() : 0;
        {
            std::unique_lock lock(m_mutex);
            auto& s = m_slots[slot];
            s.agent = agent;
            s.agentFormID = newFormId;
            s.state = state;
            s.taskType = taskType;
            s.targetName = targetName;
        }

        // Busy state stays in this task tracker: SkyrimNet's global API has no owner token.

        logger::debug("SlotTracker: Updated slot {} -> agent={}, state={}, type={}, target={}",
                     slot, agent ? agent->GetDisplayFullName() : "null", state, taskType, targetName);
    }

    void SlotTracker::ClearSlot(int slot) {
        if (slot < 0 || slot >= MAX_SLOTS) return;

        {
            std::unique_lock lock(m_mutex);
            auto& s = m_slots[slot];
            s.agent = nullptr;
            s.agentFormID = 0;
            s.state = 0;
            s.taskType.clear();
            s.targetName.clear();
            s.speed = 0;
            s.deadline = 0.0f;
            s.offscreenArrival = 0.0f;
            // Note: cooldown is NOT cleared here — it's per-actor, not per-slot
        }

        logger::debug("SlotTracker: Cleared slot {}", slot);
    }

    void SlotTracker::SetCooldown(RE::Actor* actor, float durationSeconds) {
        if (!actor) return;

        std::unique_lock lock(m_mutex);

        float now = GetRealTimeSeconds();
        float expiry = now + durationSeconds;

        // Store on the slot data if actor has an active slot
        for (auto& s : m_slots) {
            if (s.agent == actor) {
                s.cooldownExpiry = expiry;
                break;
            }
        }

        // Also store in a separate map for post-clear lookups
        m_cooldowns[actor->GetFormID()] = expiry;

        logger::debug("SlotTracker: Cooldown set for {} -> {}s", actor->GetDisplayFullName(), durationSeconds);
    }

    bool SlotTracker::HasActiveTask(RE::Actor* actor) const {
        if (!actor) return false;

        std::shared_lock lock(m_mutex);
        for (const auto& s : m_slots) {
            if (s.agent == actor && s.state != 0) {
                return true;
            }
        }
        return false;
    }

    bool SlotTracker::IsOnCooldown(RE::Actor* actor) const {
        if (!actor) return false;

        std::shared_lock lock(m_mutex);

        float now = GetRealTimeSeconds();

        // Check per-slot cooldown
        for (const auto& s : m_slots) {
            if (s.agent == actor && s.cooldownExpiry > now) {
                return true;
            }
        }

        // Check post-clear cooldown map
        auto it = m_cooldowns.find(actor->GetFormID());
        if (it != m_cooldowns.end() && it->second > now) {
            return true;
        }

        return false;
    }

    std::string SlotTracker::GetTaskType(RE::Actor* actor) const {
        if (!actor) return "";

        std::shared_lock lock(m_mutex);
        for (const auto& s : m_slots) {
            if (s.agent == actor && s.state != 0) {
                return s.taskType;
            }
        }
        return "";
    }

    std::string SlotTracker::GetTargetName(RE::Actor* actor) const {
        if (!actor) return "";

        std::shared_lock lock(m_mutex);
        for (const auto& s : m_slots) {
            if (s.agent == actor && s.state != 0) {
                return s.targetName;
            }
        }
        return "";
    }

    int SlotTracker::FindSlotByActor(RE::Actor* actor) const {
        if (!actor) return -1;

        std::shared_lock lock(m_mutex);
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (m_slots[i].agent == actor) {
                return i;
            }
        }
        return -1;
    }

    std::string SlotTracker::SerializeToJson() const {
        std::shared_lock lock(m_mutex);

        float now = GetRealTimeSeconds();
        nlohmann::json slots = nlohmann::json::array();

        for (int i = 0; i < MAX_SLOTS; ++i) {
            const auto& s = m_slots[i];
            nlohmann::json slot;
            slot["index"] = i;
            slot["state"] = s.state;
            slot["taskType"] = s.taskType;
            slot["targetName"] = s.targetName;
            slot["agentName"] = (s.agent && s.state != 0)
                ? s.agent->GetDisplayFullName() : "";
            slot["agentFormId"] = (s.agent && s.state != 0)
                ? static_cast<int>(s.agent->GetFormID()) : 0;

            // Remaining cooldown in seconds (0 if not on cooldown)
            float cooldownRemaining = 0.0f;
            if (s.agent && s.cooldownExpiry > now) {
                cooldownRemaining = s.cooldownExpiry - now;
            } else if (s.agent) {
                auto it = m_cooldowns.find(s.agent->GetFormID());
                if (it != m_cooldowns.end() && it->second > now) {
                    cooldownRemaining = it->second - now;
                }
            }
            slot["cooldownRemaining"] = cooldownRemaining;

            slots.push_back(slot);
        }

        return slots.dump();
    }

    void SlotTracker::ClearAll() {
        // Save switching clears only our own volatile memory.
        {
            std::unique_lock lock(m_mutex);
            for (auto& s : m_slots) {
                s.agent = nullptr;
                s.agentFormID = 0;
                s.state = 0;
                s.taskType.clear();
                s.targetName.clear();
                s.cooldownExpiry = 0.0f;
                s.speed = 0;
                s.deadline = 0.0f;
                s.offscreenArrival = 0.0f;
            }
            m_cooldowns.clear();
            m_hasCoSaveData.store(false, std::memory_order_release);
        }
        logger::info("SlotTracker: Volatile task state cleared" );
    }

    std::string SlotTracker::BuildBusyReason(const std::string& taskType, const std::string& targetName) {
        if (taskType == "travel") return "traveling to " + targetName;
        if (taskType == "fetch_npc") return "fetching " + targetName;
        if (taskType == "deliver_message") return "delivering a message to " + targetName;
        if (taskType == "escort_target") return "escorting " + targetName;
        if (taskType == "search_for_actor") return "searching for " + targetName;
        if (taskType == "assassination") return "on an assassination task";
        if (taskType == "npc_social") return "going to talk with " + targetName;
        if (taskType == "story") return "heading to speak with " + targetName;
        if (taskType == "story_npc") return "going to meet " + targetName;
        return taskType + ": " + targetName;
    }

    // --- Persistence ---

    static constexpr uint32_t SLOT_RECORD_TYPE = 'IETK';
    void SlotTracker::Save(SKSE::SerializationInterface* a_intfc) {
        static_assert(MAX_SLOTS == Persistence::SLOT_COUNT);
        std::array<Persistence::SavedSlot, Persistence::SLOT_COUNT> snapshot;
        {
            std::shared_lock lock(m_mutex);
            for (size_t i = 0; i < snapshot.size(); ++i) {
                const auto& s = m_slots[i];
                snapshot[i] = {s.agentFormID, s.state, s.taskType, s.targetName,
                    s.speed, s.deadline, s.offscreenArrival};
            }
        }
        std::vector<unsigned char> bytes;
        if (!Persistence::EncodeSlots(snapshot, bytes)) {
            logger::error("SlotTracker::Save: Invalid runtime slot; no partial IETK record written");
            return;
        }
        if (!a_intfc->OpenRecord(SLOT_RECORD_TYPE, Persistence::SLOT_RECORD_VERSION) ||
            !a_intfc->WriteRecordData(bytes.data(), static_cast<uint32_t>(bytes.size()))) {
            logger::error("SlotTracker::Save: Failed to write IETK record");
        }
    }

    void SlotTracker::Load(SKSE::SerializationInterface* a_intfc, uint32_t version, uint32_t length) {
        if (m_hasCoSaveData.load(std::memory_order_acquire)) {
            logger::error("SlotTracker::Load: Duplicate IETK record ignored");
            return;
        }
        if (version != Persistence::SLOT_RECORD_VERSION || length > Persistence::MAX_SLOT_RECORD_BYTES) {
            logger::error("SlotTracker::Load: Unsupported IETK version/length ({}/{})", version, length);
            return;
        }
        std::vector<unsigned char> bytes(length);
        std::array<Persistence::SavedSlot, Persistence::SLOT_COUNT> raw;
        if (a_intfc->ReadRecordData(bytes.data(), length) != length ||
            !Persistence::DecodeSlots(bytes, version, raw)) {
            logger::error("SlotTracker::Load: Invalid or truncated IETK; keeping legacy recovery available");
            return;
        }
        // Decode and remap into temporary state. Nothing becomes authoritative
        // until the entire versioned record is valid. Removed actors are skipped;
        // existing plugin load-order changes are resolved by SKSE's mapping.
        std::array<SlotData, MAX_SLOTS> restored{};
        std::unordered_map<RE::FormID, bool> seenActors;
        int recovered = 0;
        for (int i = 0; i < MAX_SLOTS; ++i) {
            const auto& r = raw[i];
            if (r.state == 0) continue;
            uint32_t formID = 0;
            if (!a_intfc->ResolveFormID(r.formID, formID)) {
                logger::warn("SlotTracker::Load: Missing actor form {:X} in slot {}", r.formID, i);
                continue;
            }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);
            if (!actor || actor->IsDead()) continue;
            if (!seenActors.emplace(formID, true).second) {
                logger::error("SlotTracker::Load: Actor {:X} occurs in multiple slots; rejecting record", formID);
                return;
            }
            auto& s = restored[i];
            s.agent = actor;
            s.agentFormID = formID;
            s.state = r.state;
            s.taskType = r.taskType;
            s.targetName = r.targetName;
            s.speed = r.speed;
            s.deadline = r.deadline;
            s.offscreenArrival = r.offscreen;
            ++recovered;
        }
        {
            std::unique_lock lock(m_mutex);
            m_slots = std::move(restored);
            m_hasCoSaveData.store(true, std::memory_order_release);
        }
        logger::info("SlotTracker::Load: Atomically recovered {} task slots", recovered);
    }

    // --- Per-field setters ---

    void SlotTracker::SetSlotSpeed(int slot, int speed) {
        if (slot < 0 || slot >= MAX_SLOTS) return;
        std::unique_lock lock(m_mutex);
        m_slots[slot].speed = speed;
    }

    void SlotTracker::SetSlotDeadline(int slot, float deadline) {
        if (slot < 0 || slot >= MAX_SLOTS) return;
        std::unique_lock lock(m_mutex);
        m_slots[slot].deadline = deadline;
    }

    void SlotTracker::SetSlotOffscreenArrival(int slot, float arrival) {
        if (slot < 0 || slot >= MAX_SLOTS) return;
        std::unique_lock lock(m_mutex);
        m_slots[slot].offscreenArrival = arrival;
    }

}  // namespace IntelEngine


