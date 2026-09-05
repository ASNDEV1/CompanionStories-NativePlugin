#pragma once

// Deterministic, save-owned personal stories. The adapter supplies verified game
// observations; prose and model responses are never evidence of an accomplishment.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace IntelEngine::PersonalStories {
    inline constexpr uint32_t RECORD_TYPE = 0x49455047; // IEPG
    inline constexpr uint32_t RECORD_VERSION = 1;
    inline constexpr size_t MAX_STORIES = 16;
    inline constexpr size_t MAX_PARTICIPANTS = 8;
    inline constexpr size_t MAX_ID_BYTES = 64;
    inline constexpr size_t MAX_REASON_BYTES = 512;
    inline constexpr size_t MAX_AFTERMATH_BYTES = 1024;
    inline constexpr size_t MAX_RECORD_BYTES = 64 * 1024;

    enum class Kind : uint32_t { Expedition, Gathering, Reunion };
    enum class Phase : uint32_t { Proposed, Active, Paused, ReadyForReflection, Completed, Declined, Failed };
    enum class Outcome : uint32_t { None, ExpeditionCleared, ExpeditionVisited, GatheringHeld, ReunionConversation };
    enum class Observation : uint32_t {
        EnteredTogether, ClearedTogether, ExitedTogether, SuppliesObserved,
        RequiredParticipantsPresent, TwoWayDialogue, ReflectionDialogue, Count
    };
    enum class Failure : uint32_t { None, OwnerDied, TargetDied };
    enum class Result { Applied, NoChange, StaleRevision, InvalidTransition, InvalidEvidence, InvalidData };

    struct Evidence {
        Observation type = Observation::EnteredTogether;
        uint64_t eventId = 0; // Nonzero stable identity of the observed game event.
        float gameTime = 0; // GameDaysPassed, supplied by the game adapter.
    };

    struct Story {
        std::string id; // Stable configuration ID; never an actor display name.
        Kind kind = Kind::Expedition;
        Phase phase = Phase::Proposed;
        uint32_t owner = 0;
        std::vector<uint32_t> participants;
        uint32_t destination = 0; // Resolved reference/location FormID; adapter defines its meaning.
        uint32_t target = 0; // Reunion counterpart; zero when unused.
        uint32_t milestones = 0;
        float offeredAt = 0;
        float acceptedAt = 0;
        float deadline = 0; // Zero means no appointment; expiry never fabricates failure.
        float lastEvidenceAt = 0;
        uint64_t revision = 1;
        uint64_t outcomeRevision = 0;
        Outcome outcome = Outcome::None;
        Failure failure = Failure::None;
        std::string reason;
        std::string aftermath;
        std::array<uint64_t, static_cast<size_t>(Observation::Count)> evidenceIds{};
    };
    struct Store { std::vector<Story> stories; };

    inline constexpr uint32_t Bit(Observation type) { return 1u << static_cast<uint32_t>(type); }
    inline bool Has(const Story& story, Observation type) { return (story.milestones & Bit(type)) != 0; }
    inline bool ValidTime(float value) { return std::isfinite(value) && value >= 0; }
    inline bool ValidText(const std::string& value, size_t bound) {
        return value.size() <= bound && value.find('\0') == std::string::npos;
    }
    inline bool ValidParticipants(const std::vector<uint32_t>& participants, uint32_t owner) {
        if (participants.size() > MAX_PARTICIPANTS) return false;
        std::unordered_set<uint32_t> seen;
        for (auto actor : participants) if (!actor || actor == owner || !seen.insert(actor).second) return false;
        return true;
    }
    inline bool Valid(const Story& s) {
        if (s.id.empty() || !ValidText(s.id, MAX_ID_BYTES) || !s.owner ||
            !ValidText(s.reason, MAX_REASON_BYTES) || !ValidText(s.aftermath, MAX_AFTERMATH_BYTES) ||
            !ValidParticipants(s.participants, s.owner) || !ValidTime(s.offeredAt) ||
            !ValidTime(s.acceptedAt) || !ValidTime(s.deadline) || !ValidTime(s.lastEvidenceAt) ||
            (s.deadline && s.deadline < s.offeredAt) ||
            s.revision == 0 || s.outcomeRevision > s.revision ||
            static_cast<uint32_t>(s.kind) > static_cast<uint32_t>(Kind::Reunion) ||
            static_cast<uint32_t>(s.phase) > static_cast<uint32_t>(Phase::Failed) ||
            static_cast<uint32_t>(s.outcome) > static_cast<uint32_t>(Outcome::ReunionConversation) ||
            static_cast<uint32_t>(s.failure) > static_cast<uint32_t>(Failure::TargetDied)) return false;
        if (s.kind == Kind::Reunion && (!s.target || s.target == s.owner)) return false;
        if (s.target == s.owner) return false;
        const uint32_t allowed = s.kind == Kind::Expedition
            ? Bit(Observation::EnteredTogether) | Bit(Observation::ClearedTogether) | Bit(Observation::ExitedTogether)
            : s.kind == Kind::Gathering
                ? Bit(Observation::SuppliesObserved) | Bit(Observation::RequiredParticipantsPresent) | Bit(Observation::TwoWayDialogue)
                : Bit(Observation::RequiredParticipantsPresent) | Bit(Observation::TwoWayDialogue);
        if (s.milestones & ~(allowed | Bit(Observation::ReflectionDialogue))) return false;
        for (size_t i = 0; i < s.evidenceIds.size(); ++i)
            if (bool(s.milestones & (1u << i)) != bool(s.evidenceIds[i])) return false;
        const bool concluded = s.phase == Phase::ReadyForReflection || s.phase == Phase::Completed;
        if (concluded != (s.outcome != Outcome::None) || concluded != (s.outcomeRevision != 0)) return false;
        if ((s.phase == Phase::Failed) != (s.failure != Failure::None)) return false;
        if ((s.phase == Phase::Completed) != Has(s, Observation::ReflectionDialogue)) return false;
        if (s.phase != Phase::Completed && !s.aftermath.empty()) return false;
        if (s.phase == Phase::Proposed || s.phase == Phase::Declined) {
            if (s.milestones || s.acceptedAt || s.lastEvidenceAt) return false;
        } else if (s.phase != Phase::Failed) {
            if (s.acceptedAt < s.offeredAt || (!s.destination && s.kind != Kind::Reunion) ||
                (s.participants.empty() && s.kind != Kind::Reunion)) return false;
        }
        if (s.milestones && s.lastEvidenceAt < s.acceptedAt) return false;
        if (Has(s, Observation::ClearedTogether) || Has(s, Observation::ExitedTogether)) {
            if (!Has(s, Observation::EnteredTogether)) return false;
        }
        if (Has(s, Observation::TwoWayDialogue) && !Has(s, Observation::RequiredParticipantsPresent)) return false;
        if (s.kind == Kind::Gathering && Has(s, Observation::RequiredParticipantsPresent) && !Has(s, Observation::SuppliesObserved)) return false;
        if (Has(s, Observation::ClearedTogether) && Has(s, Observation::ExitedTogether)) return false;
        const bool finished = s.kind == Kind::Expedition
            ? Has(s, Observation::ClearedTogether) || Has(s, Observation::ExitedTogether)
            : Has(s, Observation::TwoWayDialogue);
        if (finished != concluded) return false;
        if (s.outcome == Outcome::ExpeditionCleared && (s.kind != Kind::Expedition || !Has(s, Observation::ClearedTogether))) return false;
        if (s.outcome == Outcome::ExpeditionVisited && (s.kind != Kind::Expedition || !Has(s, Observation::ExitedTogether))) return false;
        if (s.outcome == Outcome::GatheringHeld && (s.kind != Kind::Gathering || !Has(s, Observation::TwoWayDialogue))) return false;
        if (s.outcome == Outcome::ReunionConversation && (s.kind != Kind::Reunion || !Has(s, Observation::TwoWayDialogue))) return false;
        return true;
    }

    inline Result CheckRevision(const Story& story, uint64_t expected) {
        if (!Valid(story) || story.revision == std::numeric_limits<uint64_t>::max()) return Result::InvalidData;
        return story.revision == expected ? Result::Applied : Result::StaleRevision;
    }
    inline Story* Find(Store& store, const std::string& id) {
        for (auto& story : store.stories) if (story.id == id) return &story;
        return nullptr;
    }
    inline Result Offer(Store& store, Story story) {
        if (Find(store, story.id)) return Result::NoChange; // Includes completed and declined projects.
        if (store.stories.size() >= MAX_STORIES || !Valid(story) || story.phase != Phase::Proposed)
            return Result::InvalidData;
        store.stories.push_back(std::move(story));
        return Result::Applied;
    }
    inline Result Accept(Story& story, uint64_t expectedRevision, float gameTime) {
        auto result = CheckRevision(story, expectedRevision);
        if (result != Result::Applied) return result;
        if (story.phase != Phase::Proposed) return Result::InvalidTransition;
        if (!ValidTime(gameTime) || gameTime < story.offeredAt ||
            (story.participants.empty() && story.kind != Kind::Reunion) ||
            (!story.destination && story.kind != Kind::Reunion)) return Result::InvalidData;
        story.phase = Phase::Active;
        story.acceptedAt = gameTime;
        ++story.revision;
        return Result::Applied;
    }
    inline Result Pause(Story& story, uint64_t expectedRevision) {
        auto result = CheckRevision(story, expectedRevision);
        if (result != Result::Applied) return result;
        if (story.phase == Phase::Paused) return Result::NoChange;
        if (story.phase != Phase::Active) return Result::InvalidTransition;
        story.phase = Phase::Paused; ++story.revision;
        return Result::Applied;
    }
    inline Result Resume(Story& story, uint64_t expectedRevision) {
        auto result = CheckRevision(story, expectedRevision);
        if (result != Result::Applied) return result;
        if (story.phase != Phase::Paused) return Result::InvalidTransition;
        story.phase = Phase::Active; ++story.revision;
        return Result::Applied;
    }
    inline Result Defer(Story& story, uint64_t expectedRevision, float newDeadline) {
        auto result = CheckRevision(story, expectedRevision);
        if (result != Result::Applied) return result;
        if (story.phase != Phase::Proposed && story.phase != Phase::Active && story.phase != Phase::Paused)
            return Result::InvalidTransition;
        if (!ValidTime(newDeadline) || (newDeadline && newDeadline < story.offeredAt)) return Result::InvalidData;
        if (newDeadline == story.deadline) return Result::NoChange;
        story.deadline = newDeadline; ++story.revision;
        return Result::Applied;
    }
    inline Result Decline(Story& story, uint64_t expectedRevision) {
        auto result = CheckRevision(story, expectedRevision);
        if (result != Result::Applied) return result;
        if (story.phase != Phase::Proposed) return Result::InvalidTransition;
        story.phase = Phase::Declined; ++story.revision;
        return Result::Applied;
    }
    inline Result Fail(Story& story, uint64_t expectedRevision, Failure fact) {
        auto result = CheckRevision(story, expectedRevision);
        if (result != Result::Applied) return result;
        if (story.phase != Phase::Proposed && story.phase != Phase::Active && story.phase != Phase::Paused)
            return Result::InvalidTransition;
        if (fact != Failure::OwnerDied && !(fact == Failure::TargetDied && story.target)) return Result::InvalidEvidence;
        story.phase = Phase::Failed; story.failure = fact; ++story.revision;
        return Result::Applied;
    }

    inline bool ValidEvidence(const Story& story, const Evidence& event) {
        return event.eventId != 0 && ValidTime(event.gameTime) && event.gameTime >= story.acceptedAt &&
            event.gameTime >= story.lastEvidenceAt &&
            static_cast<uint32_t>(event.type) < static_cast<uint32_t>(Observation::Count);
    }
    inline void Record(Story& story, const Evidence& event) {
        story.milestones |= Bit(event.type);
        story.evidenceIds[static_cast<size_t>(event.type)] = event.eventId;
        story.lastEvidenceAt = event.gameTime;
        ++story.revision;
    }
    inline Result Observe(Story& story, const Evidence& event, uint64_t expectedRevision) {
        if (!Valid(story) || !event.eventId || !ValidTime(event.gameTime) ||
            static_cast<uint32_t>(event.type) >= static_cast<uint32_t>(Observation::Count) ||
            event.type == Observation::ReflectionDialogue)
            return Result::InvalidEvidence;
        // A repeated milestone never advances the revision, even when the callback
        // retains its original revision after the first application.
        if (Has(story, event.type)) return Result::NoChange;
        if (!ValidEvidence(story, event)) return Result::InvalidEvidence;
        auto result = CheckRevision(story, expectedRevision);
        if (result != Result::Applied) return result;
        if (story.phase != Phase::Active) return Result::InvalidTransition;
        bool permitted = false;
        Outcome outcome = Outcome::None;
        if (story.kind == Kind::Expedition) {
            permitted = event.type == Observation::EnteredTogether;
            if (Has(story, Observation::EnteredTogether) && event.type == Observation::ClearedTogether) {
                permitted = true; outcome = Outcome::ExpeditionCleared;
            } else if (Has(story, Observation::EnteredTogether) && event.type == Observation::ExitedTogether) {
                permitted = true; outcome = Outcome::ExpeditionVisited;
            }
        } else {
            permitted = story.kind == Kind::Gathering && event.type == Observation::SuppliesObserved;
            if (event.type == Observation::RequiredParticipantsPresent)
                permitted = story.kind == Kind::Reunion || Has(story, Observation::SuppliesObserved);
            if (event.type == Observation::TwoWayDialogue && Has(story, Observation::RequiredParticipantsPresent)) {
                permitted = true;
                outcome = story.kind == Kind::Gathering ? Outcome::GatheringHeld : Outcome::ReunionConversation;
            }
        }
        if (!permitted) return Result::InvalidEvidence;
        Record(story, event);
        if (outcome != Outcome::None) {
            story.outcome = outcome;
            story.outcomeRevision = story.revision;
            story.phase = Phase::ReadyForReflection;
        }
        return Result::Applied;
    }
    inline Result Reflect(Story& story, const Evidence& conversation, uint64_t expectedRevision,
                          const std::string& aftermath) {
        if (!ValidEvidence(story, conversation) || conversation.type != Observation::ReflectionDialogue ||
            !ValidText(aftermath, MAX_AFTERMATH_BYTES)) return Result::InvalidEvidence;
        if (story.phase == Phase::Completed && Has(story, Observation::ReflectionDialogue)) return Result::NoChange;
        auto result = CheckRevision(story, expectedRevision);
        if (result != Result::Applied) return result;
        if (story.phase != Phase::ReadyForReflection) return Result::InvalidTransition;
        // The encounter that made reflection available is not itself a later
        // reflection exchange. Require a distinct observed conversation event.
        if (std::find(story.evidenceIds.begin(), story.evidenceIds.end(), conversation.eventId) != story.evidenceIds.end())
            return Result::InvalidEvidence;
        Record(story, conversation);
        story.aftermath = aftermath;
        story.phase = Phase::Completed;
        return Result::Applied;
    }

    // Called by the SKSE adapter using ResolveFormID. A failed resolution publishes
    // nothing; the adapter can retain the original record inert rather than save
    // partially remapped IDs. No engine pointer is persisted.
    template<class Resolver> bool RemapForms(Store& store, Resolver resolve) {
        Store remapped = store;
        for (auto& story : remapped.stories) {
            auto remap = [&](uint32_t& id) {
                if (!id) return true;
                uint32_t current = 0;
                if (!resolve(id, current) || !current) return false;
                id = current;
                return true;
            };
            if (!remap(story.owner) || !remap(story.destination) || !remap(story.target)) return false;
            for (auto& participant : story.participants) if (!remap(participant)) return false;
            if (!Valid(story)) return false;
        }
        store = std::move(remapped);
        return true;
    }

    inline bool Encode(const Store& store, std::vector<unsigned char>& output) {
        if (store.stories.size() > MAX_STORIES) return false;
        std::unordered_set<std::string> ids;
        std::vector<unsigned char> bytes;
        auto number = [&]<class T>(const T& value) {
            auto* first = reinterpret_cast<const unsigned char*>(&value);
            bytes.insert(bytes.end(), first, first + sizeof(T));
        };
        auto text = [&](const std::string& value) {
            number(static_cast<uint32_t>(value.size()));
            bytes.insert(bytes.end(), value.begin(), value.end());
        };
        number(static_cast<uint32_t>(store.stories.size()));
        for (const auto& s : store.stories) {
            if (!Valid(s) || !ids.insert(s.id).second) return false;
            text(s.id); number(s.kind); number(s.phase); number(s.owner);
            number(static_cast<uint32_t>(s.participants.size()));
            for (auto actor : s.participants) number(actor);
            number(s.destination); number(s.target); number(s.milestones);
            number(s.offeredAt); number(s.acceptedAt); number(s.deadline); number(s.lastEvidenceAt);
            number(s.revision); number(s.outcomeRevision); number(s.outcome); number(s.failure);
            text(s.reason); text(s.aftermath);
            for (auto id : s.evidenceIds) number(id);
        }
        if (bytes.size() > MAX_RECORD_BYTES) return false;
        output = std::move(bytes);
        return true;
    }
    inline bool Decode(std::span<const unsigned char> bytes, uint32_t version, Store& output) {
        if (version != RECORD_VERSION || bytes.size() > MAX_RECORD_BYTES) return false;
        size_t offset = 0;
        auto number = [&]<class T>(T& value) {
            if (sizeof(T) > bytes.size() - offset) return false;
            std::memcpy(&value, bytes.data() + offset, sizeof(T)); offset += sizeof(T);
            return true;
        };
        auto text = [&](std::string& value, size_t bound) {
            uint32_t length = 0;
            if (!number(length) || length > bound || length > bytes.size() - offset) return false;
            value.assign(reinterpret_cast<const char*>(bytes.data() + offset), length);
            offset += length;
            return true;
        };
        uint32_t count = 0;
        if (!number(count) || count > MAX_STORIES) return false;
        Store restored;
        std::unordered_set<std::string> ids;
        for (uint32_t i = 0; i < count; ++i) {
            Story s;
            uint32_t participants = 0;
            if (!text(s.id, MAX_ID_BYTES) || !number(s.kind) || !number(s.phase) || !number(s.owner) ||
                !number(participants) || participants > MAX_PARTICIPANTS) return false;
            s.participants.resize(participants);
            for (auto& actor : s.participants) if (!number(actor)) return false;
            if (!number(s.destination) || !number(s.target) || !number(s.milestones) ||
                !number(s.offeredAt) || !number(s.acceptedAt) || !number(s.deadline) || !number(s.lastEvidenceAt) ||
                !number(s.revision) || !number(s.outcomeRevision) || !number(s.outcome) || !number(s.failure) ||
                !text(s.reason, MAX_REASON_BYTES) || !text(s.aftermath, MAX_AFTERMATH_BYTES)) return false;
            for (auto& id : s.evidenceIds) if (!number(id)) return false;
            if (!Valid(s) || !ids.insert(s.id).second) return false;
            restored.stories.push_back(std::move(s));
        }
        if (offset != bytes.size()) return false;
        output = std::move(restored);
        return true;
    }
}
