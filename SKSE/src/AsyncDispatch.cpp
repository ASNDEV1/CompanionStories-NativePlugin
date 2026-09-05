#include "AsyncDispatch.h"
#include "Plugin.h"
#include <array>
#include <chrono>
#include <random>

namespace IntelEngine::AsyncDispatch {

    namespace {
        std::mutex g_mutex;
        std::condition_variable g_cv;
        std::queue<std::function<void()>> g_queue;
        std::thread g_worker;
        std::atomic<bool> g_running{false};
        bool g_sessionReady = false;
        std::uint64_t g_epoch = 0;
        std::array<std::uint64_t, 3> g_sequences{};

        void WorkerLoop() {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_cv.wait(lock, [] { return !g_queue.empty() || !g_running.load(std::memory_order_acquire); });
                    if (!g_running.load(std::memory_order_acquire)) return;
                    task = std::move(g_queue.front());
                    g_queue.pop();
                }
                try {
                    task();
                } catch (const std::exception& e) {
                    logger::error("AsyncDispatch worker: exception: {}", e.what());
                } catch (...) {
                    logger::error("AsyncDispatch worker: unknown exception");
                }
            }
        }
    }  // namespace

    void Initialize() {
        bool expected = false;
        if (!g_running.compare_exchange_strong(expected, true)) return;
        {
            std::lock_guard lock(g_mutex);
            // A Papyrus callback can itself be saved. Restarting the process
            // must not repeat epoch 2 and accidentally validate that old receipt.
            // Keep epochs exactly representable by JSON consumers using doubles.
            constexpr auto mask = (std::uint64_t{1} << 52) - 1;
            auto seed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            try {
                std::random_device random;
                seed ^= (static_cast<std::uint64_t>(random()) << 32) | random();
            } catch (...) { /* The clock still separates process starts. */ }
            g_epoch = (seed & mask) + 1;
        }
        g_worker = std::thread(WorkerLoop);
        logger::info("AsyncDispatch: worker started");
    }

    void Shutdown() {
        bool expected = true;
        if (!g_running.compare_exchange_strong(expected, false)) return;
        InvalidateSession();
        g_cv.notify_all();
        if (g_worker.joinable()) g_worker.join();
        std::lock_guard<std::mutex> lock(g_mutex);
        std::queue<std::function<void()>> empty;
        g_queue.swap(empty);
        logger::info("AsyncDispatch: worker stopped");
    }

    void InvalidateSession() {
        std::lock_guard lock(g_mutex);
        g_sessionReady = false;
        ++g_epoch;
        g_sequences.fill(0);
        std::queue<std::function<void()>> empty;
        g_queue.swap(empty);
    }

    void ResumeSession() {
        std::lock_guard lock(g_mutex);
        g_sessionReady = true;
    }

    RequestToken BeginRequest(Lane lane) {
        std::lock_guard lock(g_mutex);
        if (!g_sessionReady || !g_running.load(std::memory_order_acquire)) return {};
        return {g_epoch, ++g_sequences[static_cast<std::size_t>(lane)], lane};
    }

    bool IsCurrent(RequestToken token) {
        std::lock_guard lock(g_mutex);
        return g_sessionReady && g_running.load(std::memory_order_acquire) &&
            token.epoch == g_epoch && token.sequence != 0 &&
            token.sequence == g_sequences[static_cast<std::size_t>(token.lane)];
    }

    std::uint64_t CurrentSessionEpoch() {
        std::lock_guard lock(g_mutex);
        return g_sessionReady && g_running.load(std::memory_order_acquire) ? g_epoch : 0;
    }

    bool IsSessionCurrent(std::uint64_t epoch) {
        return epoch != 0 && epoch == CurrentSessionEpoch();
    }

    void Submit(RequestToken token, std::function<void()> work) {
        if (!IsCurrent(token)) return;
        Submit([token, work = std::move(work)]() {
            if (IsCurrent(token)) work();
        });
    }

    void Submit(std::function<void()> work) {
        // Recheck g_running under the lock — closes the TOCTOU between an unlocked
        // load() and the queue push if Shutdown() runs concurrently. AsyncDispatch
        // is initialized once in SKSEPluginLoad before any Submit can happen, so
        // the not-initialized branch is a hard error not a fallback.
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_running.load(std::memory_order_acquire)) {
                logger::error("AsyncDispatch::Submit before Initialize or after Shutdown — work dropped");
                return;
            }
            g_queue.push(std::move(work));
        }
        g_cv.notify_one();
    }

    // -------------------------------------------------------------------------
    // ExecuteQuestFunctionString — mirror of SkyrimNet's Papyrus::ExecuteQuestFunction
    // pattern, trimmed to a single-String argument shape.
    // Source reference: Mods/SkyrimNet/src/Skyrim/Papyrus/PapyrusQuestScript.cpp:130-205
    // -------------------------------------------------------------------------

    static RE::TESQuest* FindQuestByEditorID(const std::string& editorId) {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return nullptr;
        auto& quests = dataHandler->GetFormArray<RE::TESQuest>();
        for (auto* quest : quests) {
            if (quest && quest->GetFormEditorID() == editorId) return quest;
        }
        return nullptr;
    }

    bool ExecuteQuestFunctionString(const std::string& questEditorId,
                                    const std::string& scriptName,
                                    const std::string& functionName,
                                    const std::string& stringArg) {
        try {
            auto* quest = FindQuestByEditorID(questEditorId);
            if (!quest) {
                logger::error("ExecuteQuestFunctionString: quest '{}' not found", questEditorId);
                return false;
            }

            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                logger::error("ExecuteQuestFunctionString: VM not available");
                return false;
            }

            auto* policy = vm->GetObjectHandlePolicy1();
            if (!policy) {
                logger::error("ExecuteQuestFunctionString: handle policy not available");
                return false;
            }

            auto handle = policy->GetHandleForObject(RE::FormType::Quest, quest);
            if (handle == policy->EmptyHandle()) {
                logger::error("ExecuteQuestFunctionString: failed to get VM handle for quest '{}'", questEditorId);
                return false;
            }

            RE::BSTSmartPointer<RE::BSScript::Object> script;
            if (!vm->FindBoundObject(handle, scriptName.c_str(), script) || !script) {
                logger::error("ExecuteQuestFunctionString: script '{}' not bound on quest '{}'",
                              scriptName, questEditorId);
                return false;
            }

            auto* args = RE::MakeFunctionArguments(RE::BSFixedString(stringArg.c_str()));
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            RE::BSFixedString fnName(functionName.c_str());

            bool dispatched = vm->DispatchMethodCall1(script, fnName, args, callback);
            if (!dispatched) {
                logger::error("ExecuteQuestFunctionString: DispatchMethodCall1 failed for {}::{}",
                              scriptName, functionName);
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            logger::error("ExecuteQuestFunctionString: exception: {}", e.what());
            return false;
        }
    }

    bool ExecuteQuestFunctionResponse(const std::string& questEditorId,
                                      const std::string& scriptName,
                                      const std::string& functionName,
                                      const std::string& response, int success) {
        try {
            auto* quest = FindQuestByEditorID(questEditorId);
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!quest || !vm) return false;
            auto* policy = vm->GetObjectHandlePolicy1();
            if (!policy) return false;
            const auto handle = policy->GetHandleForObject(RE::FormType::Quest, quest);
            if (handle == policy->EmptyHandle()) return false;
            RE::BSTSmartPointer<RE::BSScript::Object> script;
            if (!vm->FindBoundObject(handle, scriptName.c_str(), script) || !script) return false;
            auto* args = RE::MakeFunctionArguments(RE::BSFixedString(response.c_str()), static_cast<int>(success));
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            return vm->DispatchMethodCall1(script, RE::BSFixedString(functionName.c_str()), args, callback);
        } catch (const std::exception& e) {
            logger::error("ExecuteQuestFunctionResponse: {}", e.what());
            return false;
        }
    }

}  // namespace IntelEngine::AsyncDispatch

