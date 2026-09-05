/**
 * Proximity Monitor Implementation
 *
 * Main-thread distance checks at 150ms cadence. See header for rationale.
 */

#include "ProximityMonitor.h"
#include "AsyncDispatch.h"
#include "PersonalStories.h"

#include <chrono>
#include <cmath>
#include <charconv>
#include <string_view>

namespace IntelEngine {

    void ProximityMonitor::Arm(int slot, RE::TESObjectREFR* agent, RE::TESObjectREFR* target,
                               float threshold, float zTolerance,
                               const std::string& questEditorId,
                               const std::string& scriptName,
                               const std::string& callbackFn) {
        if (slot < 0 || slot >= MAX_SLOTS) {
            logger::warn("ProximityMonitor::Arm: invalid slot {}", slot);
            return;
        }
        if (!agent || !target) {
            logger::warn("ProximityMonitor::Arm: null agent or target for slot {}", slot);
            return;
        }

        // Auto-select defaults based on target kind. Actor targets need a tighter
        // conversational threshold and a tighter Z window (an NPC a floor above
        // the player shouldn't fire arrival). Marker targets (doors, furniture)
        // use the legacy Papyrus 300u radius and a looser Z window because markers
        // are often placed at floor level while the navmesh endpoint sits higher.
        const bool targetIsActor = target->As<RE::Actor>() != nullptr;
        if (threshold <= 0.0f) {
            threshold = targetIsActor ? DEFAULT_ACTOR_THRESHOLD : DEFAULT_MARKER_THRESHOLD;
        }
        if (zTolerance <= 0.0f) {
            zTolerance = targetIsActor ? DEFAULT_ACTOR_Z_TOLERANCE : DEFAULT_MARKER_Z_TOLERANCE;
        }

        const auto epoch = AsyncDispatch::CurrentSessionEpoch();
        if (!epoch) return;
        std::unique_lock lock(m_mutex);
        auto& w = m_slots[slot];
        w.epoch = epoch;
        w.generation = ++m_generations[slot];
        m_receiptEpochs[slot] = 0;
        w.armed = true;
        w.agentFormID = agent->GetFormID();
        w.targetFormID = target->GetFormID();
        w.threshold = threshold;
        w.zTolerance = zTolerance;
        w.questEditorId = questEditorId;
        w.scriptName = scriptName;
        w.callbackFn = callbackFn;

        logger::info("ProximityMonitor::Arm slot={} agent={:08X} target={:08X} "
                     "actor={} threshold={:.0f} z={:.0f} -> {}::{}",
                     slot, w.agentFormID, w.targetFormID, targetIsActor,
                     w.threshold, w.zTolerance, scriptName, callbackFn);
    }

    void ProximityMonitor::Disarm(int slot) {
        if (slot < 0 || slot >= MAX_SLOTS) return;

        std::unique_lock lock(m_mutex);
        auto& w = m_slots[slot];
        ++m_generations[slot];
        m_receiptEpochs[slot] = 0;

        logger::info("ProximityMonitor::Disarm slot={}", slot);
        w = Watch{};
    }

    void ProximityMonitor::DisarmAll() {
        std::unique_lock lock(m_mutex);
        for (int i = 0; i < MAX_SLOTS; ++i) {
            m_slots[i] = Watch{};
            ++m_generations[i];
            m_receiptEpochs[i] = 0;
        }
        m_maintenance = {};
        logger::info("ProximityMonitor::DisarmAll");
    }

    int ProximityMonitor::ConsumeReceipt(const std::string& receipt) {
        // Strict machine receipt. Old bare-slot callbacks intentionally fail closed.
        if (receipt.empty() || receipt.size() > 96) return -1;
        const auto first = receipt.find(':');
        const auto second = first == std::string::npos ? first : receipt.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos || receipt.find(':', second + 1) != std::string::npos) return -1;
        std::uint64_t slot = 0, generation = 0, epoch = 0;
        auto number = [&](size_t begin, size_t end, std::uint64_t& value) {
            if (begin == end) return false;
            const auto result = std::from_chars(receipt.data() + begin, receipt.data() + end, value);
            return result.ec == std::errc{} && result.ptr == receipt.data() + end;
        };
        if (!number(0, first, slot) || !number(first + 1, second, generation) ||
            !number(second + 1, receipt.size(), epoch) || slot >= MAX_SLOTS ||
            !AsyncDispatch::IsSessionCurrent(epoch)) return -1;
        std::unique_lock lock(m_mutex);
        if (m_generations[slot] != generation || m_receiptEpochs[slot] != epoch) return -1;
        m_receiptEpochs[slot] = 0;
        return static_cast<int>(slot);
    }

    void ProximityMonitor::DeferMaintenance(std::function<void()> callback) {
        std::unique_lock lock(m_mutex);
        m_maintenance = std::move(callback);
        m_maintenanceAt = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    }

    void ProximityMonitor::Start() {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true)) return;
        m_thread = std::thread(&ProximityMonitor::WorkerLoop, this);
        logger::info("ProximityMonitor: worker started (tick={}ms)", TICK_INTERVAL_MS);
    }

    void ProximityMonitor::Stop() {
        bool expected = true;
        if (!m_running.compare_exchange_strong(expected, false)) return;
        if (m_thread.joinable()) m_thread.join();
        DisarmAll();
    }

    void ProximityMonitor::WorkerLoop() {
        // Sleep-poll loop. Each wake submits a main-thread task. RE::* getters
        // (GetPosition, Is3DLoaded, LookupByID) are not thread-safe, so all
        // the work happens in Tick() on the main thread.
        auto nextStoryTick = std::chrono::steady_clock::now();
        while (m_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(TICK_INTERVAL_MS));
            if (!m_running.load(std::memory_order_acquire)) break;

            auto* task = SKSE::GetTaskInterface();
            if (!task) continue;
            const auto epoch = AsyncDispatch::CurrentSessionEpoch();
            if (!AsyncDispatch::IsSessionCurrent(epoch)) continue;
            {
                std::shared_lock lock(m_mutex);
                bool needed = static_cast<bool>(m_maintenance) || std::chrono::steady_clock::now() >= nextStoryTick;
                for (const auto& watch : m_slots) needed = needed || watch.armed;
                if (!needed) continue;
            }
            if (m_tickPending.exchange(true)) continue;
            const bool storyTick = std::chrono::steady_clock::now() >= nextStoryTick;
            if (storyTick) nextStoryTick = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            task->AddTask([epoch, storyTick]() {
                auto* monitor = ProximityMonitor::GetSingleton();
                monitor->m_tickPending.store(false);
                if (AsyncDispatch::IsSessionCurrent(epoch)) {
                    monitor->Tick();
                    if (storyTick) PersonalStories::Tick();
                }
            });
        }
    }

    /**
     * Per-tick distance check on the main thread.
     *
     * 1. Take a snapshot of armed slots under shared_lock (fast, no writers blocked).
     * 2. Early-out if nothing is armed — the common case is zero active dispatches.
     * 3. For each armed slot, resolve agent+target via FormID lookup (returns nullptr
     *    if the form was deleted). Skip until both references are 3D-loaded; off-screen
     *    arrival is handled by the Papyrus 3s poll's time-based estimator (OffScreenTracker).
     * 4. Compute horizontal (XY) distance and absolute Z delta. Fire when both are
     *    within the slot's threshold.
     * 5. On fire: claim the slot by clearing its armed flag under unique_lock and
     *    capture the callback, then invoke the Papyrus VM synchronously. Clearing
     *    inside the lock makes this a single-shot per Arm without needing a separate
     *    fired flag; a concurrent Arm will re-enable the slot with fresh state.
     *
     * Post-Stop() safety: the worker thread is joined, but any Tick already queued on
     * the SKSE task interface may run after Stop(). The m_running guard at entry
     * prevents that queued task from touching slot state during teardown.
     */
    void ProximityMonitor::Tick() {
        if (!m_running.load(std::memory_order_acquire)) return;

        std::function<void()> maintenance;
        {
            std::unique_lock lock(m_mutex);
            if (m_maintenance && std::chrono::steady_clock::now() >= m_maintenanceAt) {
                maintenance = std::move(m_maintenance);
                m_maintenance = {};
            }
        }
        if (maintenance) maintenance();

        std::array<Watch, MAX_SLOTS> snapshot;
        bool anyArmed = false;
        {
            std::shared_lock lock(m_mutex);
            for (int i = 0; i < MAX_SLOTS; ++i) {
                snapshot[i] = m_slots[i];
                if (snapshot[i].armed) anyArmed = true;
            }
        }
        if (!anyArmed) return;

        for (int slot = 0; slot < MAX_SLOTS; ++slot) {
            const Watch& w = snapshot[slot];
            if (!w.armed || !AsyncDispatch::IsSessionCurrent(w.epoch)) continue;

            auto* agent = RE::TESForm::LookupByID<RE::TESObjectREFR>(w.agentFormID);
            auto* target = RE::TESForm::LookupByID<RE::TESObjectREFR>(w.targetFormID);
            if (!agent || !target) continue;

            // Both must be in the rendered world. Off-screen arrival is handled by
            // Papyrus's game-time estimator in OffScreenTracker.
            if (!agent->Is3DLoaded() || !target->Is3DLoaded()) continue;

            auto aPos = agent->GetPosition();
            auto tPos = target->GetPosition();
            float dx = aPos.x - tPos.x;
            float dy = aPos.y - tPos.y;
            float horiz = std::sqrt(dx * dx + dy * dy);
            float dz = std::fabs(aPos.z - tPos.z);

            if (horiz > w.threshold || dz > w.zTolerance) continue;

            // Claim + disarm atomically; a concurrent Arm on the same slot would
            // have bumped the agentFormID, so we re-check to avoid firing a stale
            // callback against the new watch's agent.
            std::string qeid, script, fn;
            {
                std::unique_lock lk(m_mutex);
                auto& live = m_slots[slot];
                if (!live.armed || live.generation != w.generation || live.epoch != w.epoch ||
                    live.agentFormID != w.agentFormID ||
                    live.targetFormID != w.targetFormID) {
                    continue;
                }
                qeid = live.questEditorId;
                script = live.scriptName;
                fn = live.callbackFn;
                m_receiptEpochs[slot] = w.epoch;
                live = Watch{};  // single-shot per Arm
            }

            logger::info("ProximityMonitor: slot {} arrived agent={:08X} target={:08X} "
                         "horiz={:.1f}u dz={:.1f}u -> {}::{}",
                         slot, w.agentFormID, w.targetFormID, horiz, dz, script, fn);

            const auto receipt = std::to_string(slot) + ":" + std::to_string(w.generation) + ":" + std::to_string(w.epoch);
            AsyncDispatch::ExecuteQuestFunctionString(qeid, script, fn, receipt);
        }
    }

}  // namespace IntelEngine

