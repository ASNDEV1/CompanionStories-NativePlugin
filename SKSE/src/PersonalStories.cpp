#include "PersonalStories.h"
#include "AsyncDispatch.h"
#include "SkyrimNetAPI.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

namespace IntelEngine::PersonalStories {
    namespace {
        using Json = nlohmann::json;
        constexpr RE::FormID kPlayer = 0x14;
        constexpr std::uint64_t kEngineEvidence = 1ull << 63;
        constexpr std::size_t kMaximumOpaqueBytes = 1024 * 1024;
        constexpr std::size_t kMaximumOpaqueRecords = 16;
        constexpr float kAgreementAgeDays = 1.0f / 48.0f;

        struct FormSpec { std::string plugin; std::uint32_t local = 0; };
        struct Destination { std::string id, title; FormSpec form; RE::FormID resolved = 0; };
        struct Supply { FormSpec form; int count = 1; RE::FormID resolved = 0; };
        struct Config {
            std::string id, title, reason;
            Kind kind = Kind::Expedition;
            FormSpec owner, target;
            std::vector<FormSpec> participants;
            std::vector<Destination> destinations;
            std::vector<Supply> supplies;
            RE::FormID ownerID = 0, targetID = 0;
            std::vector<RE::FormID> participantIDs;
            float radius = 900.0f, gatheringHour = 19.0f, gatheringDuration = 1.0f;
            bool resolved = false;
        };
        struct Dialogue {
            std::uint64_t id = 0;
            RE::FormID origin = 0, target = 0;
            std::string text, sourceType;
            std::chrono::steady_clock::time_point received{};
            float time = 0;
            std::uint64_t serial = 0;
        };
        struct CachedActor {
            RE::FormID form = 0;
            std::string context;
            bool canPlan = false, canReflect = false;
        };
        struct RawRecord { std::uint32_t version; std::vector<unsigned char> bytes; };

        Store g_store;
        std::vector<Config> g_configs;
        std::vector<RawRecord> g_records;
        std::size_t g_recordBytes = 0, g_recordCount = 0;
        std::deque<Dialogue> g_dialogue;
        std::unordered_map<std::string, std::set<std::pair<RE::FormID, RE::FormID>>> g_exchanges;
        std::unordered_map<std::string, bool> g_clearBaseline;
        std::unordered_map<std::string, std::uint64_t> g_reflectionCutoff, g_decisionCutoff;
        std::uint64_t g_dialogueSerial = 0;
        std::unordered_map<const RE::Actor*, CachedActor> g_cache;
        std::mutex g_cacheMutex;
        bool g_blocked = false, g_ready = false, g_registered = false;
        std::uint64_t g_nextEvidence = kEngineEvidence;
        std::vector<std::uint64_t> g_eventCallbacks;
        std::chrono::steady_clock::time_point g_lastTick{};

        float Now() {
            const auto* calendar = RE::Calendar::GetSingleton();
            return calendar ? calendar->GetCurrentGameTime() : 0.0f;
        }
        RE::Actor* Actor(RE::FormID id) { return id ? RE::TESForm::LookupByID<RE::Actor>(id) : nullptr; }
        bool Present(RE::Actor* actor) {
            return actor && !actor->IsDeleted() && !actor->IsDisabled() && !actor->IsDead() &&
                actor->Is3DLoaded() && actor->GetParentCell();
        }
        bool Calm(RE::Actor* actor) {
            auto* ui = RE::UI::GetSingleton();
            return Present(actor) && !actor->IsInCombat() && actor->AsActorState() &&
                !actor->AsActorState()->IsUnconscious() &&
                actor->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kIsSleeping &&
                (!ui || !ui->GameIsPaused());
        }
        std::string Name(RE::FormID id) {
            auto* actor = Actor(id);
            const char* name = actor ? actor->GetDisplayFullName() : nullptr;
            return name && name[0] ? name : "unavailable actor";
        }
        FormSpec ParseForm(const Json& value) {
            FormSpec spec;
            spec.plugin = value.at("plugin").get<std::string>();
            const auto& local = value.at("local");
            if (local.is_string()) {
                const auto text = local.get<std::string>();
                std::size_t end = 0;
                const auto parsed = std::stoul(text, &end, 0);
                if (end != text.size() || parsed > 0xFFFFFF) throw std::runtime_error("invalid local FormID");
                spec.local = static_cast<std::uint32_t>(parsed);
            } else {
                spec.local = local.get<std::uint32_t>();
            }
            if (spec.plugin.empty() || spec.plugin.size() > 260 || spec.local == 0 || spec.local > 0xFFFFFF)
                throw std::runtime_error("invalid plugin/local form pair");
            return spec;
        }
        RE::FormID Resolve(const FormSpec& spec) {
            if (spec.plugin.empty()) return 0;
            auto* data = RE::TESDataHandler::GetSingleton();
            auto* form = data ? data->LookupForm(spec.local, spec.plugin) : nullptr;
            return form ? form->GetFormID() : 0;
        }
        bool IsDestination(RE::FormID id) {
            auto* form = RE::TESForm::LookupByID(id);
            return form && (form->As<RE::TESObjectCELL>() || form->As<RE::BGSLocation>());
        }
        bool AtDestination(RE::Actor* actor, RE::FormID id, bool interior) {
            if (!Present(actor) || !id) return false;
            const auto* cell = actor->GetParentCell();
            if (interior && !cell->IsInteriorCell()) return false;
            if (cell->GetFormID() == id) return true;
            auto* location = actor->GetCurrentLocation();
            for (int depth = 0; location && depth < 16; ++depth, location = location->parentLoc)
                if (location->GetFormID() == id) return true;
            return false;
        }
        bool Cleared(RE::FormID id) {
            const auto* location = RE::TESForm::LookupByID<RE::BGSLocation>(id);
            return location && location->IsCleared();
        }
        bool Near(RE::Actor* owner, RE::Actor* other, float radius) {
            return Present(owner) && Present(other) && owner->GetParentCell() == other->GetParentCell() &&
                owner->GetPosition().GetSquaredDistance(other->GetPosition()) <= radius * radius;
        }
        Config* Configuration(const std::string& id) {
            for (auto& config : g_configs) if (config.id == id) return &config;
            return nullptr;
        }
        bool Matches(const Story& story, const Config& config) {
            if (!config.resolved || story.kind != config.kind || story.owner != config.ownerID ||
                story.target != config.targetID || story.participants != config.participantIDs) return false;
            if (!story.destination) return story.phase == Phase::Proposed || story.kind == Kind::Reunion;
            return std::any_of(config.destinations.begin(), config.destinations.end(),
                [&](const auto& destination) { return destination.resolved == story.destination; });
        }
        bool HasSupplies(const Story& story, const Config& config) {
            auto* owner = Actor(story.owner);
            if (!owner || config.supplies.empty()) return false;
            const auto inventory = owner->GetInventoryCounts();
            for (const auto& supply : config.supplies) {
                const auto* form = RE::TESForm::LookupByID<RE::TESBoundObject>(supply.resolved);
                if (!form) return false;
                const auto found = inventory.find(const_cast<RE::TESBoundObject*>(form));
                if (found == inventory.end() || found->second < supply.count) return false;
            }
            return true;
        }
        bool Together(const Story& story, const Config& config, bool atDestination) {
            auto* owner = Actor(story.owner);
            if (!Present(owner) || (atDestination && !AtDestination(owner, story.destination, story.kind == Kind::Expedition))) return false;
            for (const auto id : story.participants) {
                auto* participant = Actor(id);
                if (!Near(owner, participant, config.radius) ||
                    (atDestination && !AtDestination(participant, story.destination, story.kind == Kind::Expedition))) return false;
            }
            return story.kind != Kind::Reunion || Near(owner, Actor(story.target), config.radius);
        }
        bool SocialReady(const Story& story, const Config& config, float now) {
            if (!Calm(Actor(story.owner))) return false;
            for (auto actor : story.participants) if (!Calm(Actor(actor))) return false;
            if (story.kind == Kind::Gathering) {
                return story.deadline > 0 && now >= story.deadline &&
                    now <= story.deadline + config.gatheringDuration / 24.0f &&
                    Together(story, config, true) && HasSupplies(story, config);
            }
            return story.kind == Kind::Reunion && Together(story, config, false);
        }
        bool ObserveFact(Story& story, Observation type, float now, std::uint64_t eventID = 0) {
            if (Has(story, type)) return false;
            if (!eventID) {
                if (g_nextEvidence == std::numeric_limits<std::uint64_t>::max()) return false;
                eventID = ++g_nextEvidence;
            }
            const auto result = Observe(story, {type, eventID, now}, story.revision);
            if (result == Result::Applied) {
                if (story.phase == Phase::ReadyForReflection) g_reflectionCutoff[story.id] = g_dialogueSerial;
                logger::info("PersonalStories: {} observed milestone {} revision {}", story.id, static_cast<int>(type), story.revision);
            }
            return result == Result::Applied;
        }
        const char* PhaseLabel(Phase phase) {
            switch (phase) {
            case Phase::Proposed: return "personal idea; not agreed";
            case Phase::Active: return "agreed and active";
            case Phase::Paused: return "paused by agreement";
            case Phase::ReadyForReflection: return "observed outcome; reflection still pending";
            case Phase::Completed: return "completed with an actual later conversation";
            case Phase::Declined: return "declined; do not ask again";
            case Phase::Failed: return "ended by an observed death";
            }
            return "unavailable";
        }
        std::string OutcomeText(const Story& story) {
            switch (story.outcome) {
            case Outcome::ExpeditionCleared: return "The owner and party entered the destination together and were present for its observed cleared transition.";
            case Outcome::ExpeditionVisited: return "The owner and party visited the destination together and left together. No dungeon-clear credit was established.";
            case Outcome::GatheringHeld: return "At the agreed appointment, the host had the required supplies and exchanged directed dialogue with every required attendee at the destination. Eating and cooking are not established by this observation.";
            case Outcome::ReunionConversation: return "The two existing people were together and exchanged actual dialogue in both directions.";
            default: return "No completed outcome has been observed.";
            }
        }
        void Publish() {
            std::unordered_map<const RE::Actor*, CachedActor> next;
            if (g_ready && !g_blocked) for (const auto& story : g_store.stories) {
                const auto* config = Configuration(story.id);
                if (!config || !Matches(story, *config)) continue;
                std::ostringstream text;
                text << "\nPersonal project: " << config->title << " [story_id=" << story.id << ", revision=" << story.revision << ", session_epoch=" << AsyncDispatch::CurrentSessionEpoch() << "]\n"
                     << "Owner: " << Name(story.owner) << ". State: " << PhaseLabel(story.phase) << ".\n"
                     << "Motivation: " << story.reason << "\n";
                if (story.phase == Phase::Proposed) {
                    if (story.revision == 1) text << "Raise this as your own concrete idea in a natural conversation. A proposal is not player agreement.\n";
                    else text << "This idea has already been proposed. Do not repeat the offer in the same conversation; wait for an explicit relevant reply. Silence is not consent.\n";
                }
                if (story.destination) {
                    for (const auto& destination : config->destinations) if (destination.resolved == story.destination)
                        text << "Chosen destination: " << destination.title << " (destination_id=" << destination.id << ").\n";
                } else if (story.kind == Kind::Expedition) {
                    text << "Available destination choices (choose one explicitly): ";
                    for (const auto& destination : config->destinations) if (destination.resolved && !Cleared(destination.resolved))
                        text << destination.title << " [destination_id=" << destination.id << "]; ";
                    text << "\n";
                }
                if (story.kind == Kind::Gathering) {
                    text << "Required attendees: ";
                    for (auto actor : story.participants) text << Name(actor) << "; ";
                    text << "\nRequired supplies in host inventory: ";
                    for (const auto& supply : config->supplies) {
                        const auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(supply.resolved);
                        text << supply.count << " " << (item ? item->GetName() : "unavailable item") << "; ";
                    }
                    if (story.deadline > 0) text << "\nAppointment game day " << std::floor(story.deadline) << ", hour " << (story.deadline - std::floor(story.deadline)) * 24.0f << ". Current game day " << Now() << ".";
                    text << "\nUse existing travel, furniture, item and cooking actions normally. Inventory possession alone does not mean a meal was cooked or eaten.\n";
                }
                if (story.kind == Kind::Reunion) text << "The person sought is " << Name(story.target) << ". Their current whereabouts are not revealed by this project. Actual proximity and dialogue determine the reunion.\n";
                text << OutcomeText(story) << "\n";
                if (!story.aftermath.empty()) text << "Durable aftermath, spoken by the owner after the outcome: " << story.aftermath << "\nLet this stated judgment or intention affect later proposals; it does not add statistics or invent another event.\n";
                if (story.phase == Phase::ReadyForReflection) text << "Discuss what this meant and what you want next. CompanionStoryReflect can record a later actual line; it cannot invent completion evidence.\n";
                text << "Models may choose motivations and speech. The game alone supplies milestones. Never announce a clear, reunion, meal, or agreement without its recorded evidence.\n";
                std::vector<RE::FormID> audience = story.participants;
                audience.push_back(story.owner);
                if (story.target) audience.push_back(story.target);
                for (const auto id : audience) {
                    auto* actor = Actor(id);
                    if (!actor || !actor->Is3DLoaded() || (id != story.owner && !Near(Actor(story.owner), actor, config->radius))) continue;
                    auto& cached = next[actor];
                    cached.form = id;
                    cached.context += text.str();
                    if (id != story.owner) continue;
                    cached.canPlan |= Calm(actor) && (story.phase == Phase::Proposed || story.phase == Phase::Active || story.phase == Phase::Paused);
                    cached.canReflect |= Calm(actor) && story.phase == Phase::ReadyForReflection;
                    bool shownDecision = false, shownReflection = false;
                    for (auto it = g_dialogue.rbegin(); it != g_dialogue.rend(); ++it) {
                        if (Now() - it->time > kAgreementAgeDays) continue;
                        const bool agreement = !shownDecision && it->origin == kPlayer && it->target == story.owner && it->serial > g_decisionCutoff[story.id];
                        const bool reflection = !shownReflection && story.phase == Phase::ReadyForReflection && it->origin == story.owner &&
                            (it->target == kPlayer || it->target == story.target || std::find(story.participants.begin(), story.participants.end(), it->target) != story.participants.end()) &&
                            it->time >= story.lastEvidenceAt && it->serial > g_reflectionCutoff[story.id] && it->id != story.evidenceIds[static_cast<std::size_t>(Observation::TwoWayDialogue)];
                        if (agreement || reflection) cached.context += "Observed " + std::string(agreement ? "player decision" : "later owner dialogue") +
                            " event_id=" + std::to_string(it->id) + ": " + it->text + "\n";
                        shownDecision |= agreement; shownReflection |= reflection;
                    }
                }
            }
            std::lock_guard lock(g_cacheMutex);
            g_cache = std::move(next);
        }

        void ReadConfig() {
            g_configs.clear();
            std::ifstream stream("Data/SKSE/Plugins/CompanionStories.json", std::ios::binary);
            if (!stream) { logger::info("PersonalStories: no CompanionStories.json configured"); return; }
            try {
                auto document = Json::parse(stream);
                const auto& stories = document.at("stories");
                if (!stories.is_array() || stories.size() > MAX_STORIES) throw std::runtime_error("stories must be an array of at most 16 entries");
                std::set<std::string> ids;
                for (const auto& entry : stories) {
                    Config config;
                    config.id = entry.at("id").get<std::string>();
                    config.title = entry.value("title", config.id);
                    config.reason = entry.value("reason", "A personal project I want to discuss.");
                    const auto kind = entry.at("kind").get<std::string>();
                    if (kind == "expedition") config.kind = Kind::Expedition;
                    else if (kind == "gathering") config.kind = Kind::Gathering;
                    else if (kind == "reunion") config.kind = Kind::Reunion;
                    else throw std::runtime_error("unknown story kind");
                    if (config.id.empty() || !ValidText(config.id, MAX_ID_BYTES) || !ValidText(config.reason, MAX_REASON_BYTES) ||
                        !ValidText(config.title, 160) || !ids.insert(config.id).second) throw std::runtime_error("invalid or duplicate story id/text");
                    config.owner = ParseForm(entry.at("owner"));
                    if (entry.contains("target")) config.target = ParseForm(entry.at("target"));
                    for (const auto& participant : entry.at("participants")) config.participants.push_back(ParseForm(participant));
                    if (config.participants.size() > MAX_PARTICIPANTS) throw std::runtime_error("too many participants");
                    if (entry.contains("destinations")) for (const auto& destination : entry.at("destinations"))
                        config.destinations.push_back({destination.at("id").get<std::string>(), destination.value("title", destination.at("id").get<std::string>()), ParseForm(destination)});
                    else if (entry.contains("destination"))
                        config.destinations.push_back({"default", entry.value("destination_title", config.title), ParseForm(entry.at("destination"))});
                    if (config.destinations.size() > 8) throw std::runtime_error("too many destinations");
                    if (entry.contains("supplies")) for (const auto& supply : entry.at("supplies")) {
                        const int count = supply.value("count", 1);
                        if (count < 1 || count > 100) throw std::runtime_error("invalid supply count");
                        config.supplies.push_back({ParseForm(supply), count});
                    }
                    if (config.supplies.size() > 12) throw std::runtime_error("too many supplies");
                    config.radius = std::clamp(entry.value("radius", 900.0f), 100.0f, 2500.0f);
                    config.gatheringHour = std::clamp(entry.value("gathering_hour", 19.0f), 0.0f, 23.99f);
                    config.gatheringDuration = std::clamp(entry.value("gathering_duration_hours", 1.0f), 0.25f, 4.0f);
                    g_configs.push_back(std::move(config));
                }
                logger::info("PersonalStories: loaded {} configurations", g_configs.size());
            } catch (const std::exception& error) {
                g_configs.clear();
                logger::error("PersonalStories config rejected: {}", error.what());
            }
        }
        bool ResolveConfig(Config& config) {
            config.ownerID = Resolve(config.owner);
            config.targetID = Resolve(config.target);
            config.participantIDs.clear();
            bool valid = Actor(config.ownerID) != nullptr;
            for (const auto& participant : config.participants) {
                auto id = Resolve(participant);
                valid &= Actor(id) != nullptr;
                config.participantIDs.push_back(id);
            }
            valid &= ValidParticipants(config.participantIDs, config.ownerID);
            valid &= std::find(config.participantIDs.begin(), config.participantIDs.end(), kPlayer) != config.participantIDs.end();
            for (auto& destination : config.destinations) {
                destination.resolved = Resolve(destination.form);
                valid &= IsDestination(destination.resolved);
            }
            for (auto& supply : config.supplies) {
                supply.resolved = Resolve(supply.form);
                valid &= RE::TESForm::LookupByID<RE::TESBoundObject>(supply.resolved) != nullptr;
            }
            if (config.kind == Kind::Reunion) valid &= Actor(config.targetID) != nullptr && config.targetID != config.ownerID;
            else valid &= !config.destinations.empty();
            if (config.kind == Kind::Gathering) valid &= !config.supplies.empty() && config.participantIDs.size() >= 2;
            config.resolved = valid;
            if (!valid) logger::warn("PersonalStories: {} unavailable; required forms/participants not resolved", config.id);
            return valid;
        }
        std::optional<Dialogue> ParseDialogue(const char* payload) {
            if (!payload) return std::nullopt;
            const std::string text(payload);
            if (text.size() > 32768) return std::nullopt;
            auto event = Json::parse(text, nullptr, false);
            if (!event.is_object()) return std::nullopt;
            Dialogue result;
            result.id = event.at("id").get<std::uint64_t>();
            result.origin = event.at("originatingActorFormId").get<RE::FormID>();
            result.target = event.at("targetActorFormId").get<RE::FormID>();
            if (!result.id || result.id >= kEngineEvidence || !result.origin || !result.target) return std::nullopt;
            auto data = event.at("data");
            if (data.is_string()) data = Json::parse(data.get<std::string>(), nullptr, false);
            if (!data.is_object()) return std::nullopt;
            result.text = data.value("dialogue", data.value("text", std::string{}));
            if (result.text.empty() || !ValidText(result.text, 2048)) return std::nullopt;
            return result;
        }
        void OnDialogueMain(Dialogue event) {
            if (!g_ready || g_blocked || !Near(Actor(event.origin), Actor(event.target), 2500.0f)) return;
            event.received = std::chrono::steady_clock::now();
            // Providers may publish a spoken line through multiple dialogue
            // event categories. Such aliases are not a second conversation.
            if (std::any_of(g_dialogue.begin(), g_dialogue.end(), [&](const auto& previous) {
                return previous.id == event.id || (previous.sourceType != event.sourceType &&
                    previous.origin == event.origin && previous.target == event.target && previous.text == event.text &&
                    event.received - previous.received < std::chrono::seconds(30));
            })) return;
            event.time = Now();
            event.serial = ++g_dialogueSerial;
            g_dialogue.push_back(event);
            while (g_dialogue.size() > 32) g_dialogue.pop_front();
            for (auto& story : g_store.stories) {
                auto* config = Configuration(story.id);
                if (!config || !Matches(story, *config) || story.phase != Phase::Active || !SocialReady(story, *config, event.time)) continue;
                const auto counterpart = event.origin == story.owner ? event.target : event.target == story.owner ? event.origin : 0;
                if (!counterpart) continue;
                if (story.kind == Kind::Reunion && counterpart != story.target) continue;
                if (story.kind == Kind::Gathering && std::find(story.participants.begin(), story.participants.end(), counterpart) == story.participants.end()) continue;
                if (story.kind == Kind::Gathering) ObserveFact(story, Observation::SuppliesObserved, event.time);
                ObserveFact(story, Observation::RequiredParticipantsPresent, event.time);
                auto& exchanges = g_exchanges[story.id];
                exchanges.emplace(event.origin, event.target);
                bool complete = true;
                auto check = [&](RE::FormID other) {
                    complete &= exchanges.contains({story.owner, other}) && exchanges.contains({other, story.owner});
                };
                if (story.kind == Kind::Reunion) check(story.target);
                else for (auto participant : story.participants) check(participant);
                if (complete) ObserveFact(story, Observation::TwoWayDialogue, event.time, event.id);
            }
            Publish();
        }
        const Dialogue* DecisionEvidence(const Story& story, const Json& parameters, bool reflection) {
            const auto id = parameters.at(reflection ? "reflection_event_id" : "agreement_event_id").get<std::uint64_t>();
            const auto quote = parameters.at(reflection ? "reflection_quote" : "agreement_quote").get<std::string>();
            if (quote.empty()) return nullptr;
            for (auto it = g_dialogue.rbegin(); it != g_dialogue.rend(); ++it) {
                const auto& event = *it;
                const bool relevant = reflection ? event.origin == story.owner &&
                    (event.target == kPlayer || event.target == story.target || std::find(story.participants.begin(), story.participants.end(), event.target) != story.participants.end())
                    : event.origin == kPlayer && event.target == story.owner;
                if (!relevant) continue;
                // Only the latest directed decision can authorize a transition;
                // an earlier yes cannot override a later no or be reused.
                if (event.id != id || event.text != quote || Now() - event.time > kAgreementAgeDays) return nullptr;
                if (!reflection && event.time >= story.offeredAt && event.serial > g_decisionCutoff[story.id]) return &event;
                if (reflection && event.time >= story.lastEvidenceAt && event.serial > g_reflectionCutoff[story.id]) return &event;
                return nullptr;
            }
            return nullptr;
        }
        void ApplyAction(RE::FormID actorID, const Json& parameters, bool reflection) {
            if (!g_ready || g_blocked || !Calm(Actor(actorID)) ||
                !AsyncDispatch::IsSessionCurrent(parameters.at("session_epoch").get<std::uint64_t>())) return;
            const auto id = parameters.at("story_id").get<std::string>();
            const auto expected = parameters.at("revision").get<std::uint64_t>();
            auto* story = Find(g_store, id);
            auto* config = Configuration(id);
            if (!story || !config || !Matches(*story, *config) || story->owner != actorID || CheckRevision(*story, expected) != Result::Applied) {
                logger::info("PersonalStories: stale or unavailable action for {}", id); return;
            }
            if (reflection) {
                if (story->phase != Phase::ReadyForReflection) return;
                const auto* evidence = DecisionEvidence(*story, parameters, true);
                if (!evidence) { logger::info("PersonalStories: {} reflection lacks actual later dialogue", id); return; }
                const auto quote = evidence->text.substr(0, MAX_AFTERMATH_BYTES);
                Reflect(*story, {Observation::ReflectionDialogue, evidence->id, evidence->time}, expected, quote);
                Publish(); return;
            }
            const auto decision = parameters.at("decision").get<std::string>();
            if (decision != "propose" && decision != "accept" && decision != "pause" && decision != "resume" && decision != "decline") return;
            const Dialogue* agreement = nullptr;
            if (decision != "propose") {
                agreement = DecisionEvidence(*story, parameters, false);
                if (!Near(Actor(actorID), Actor(kPlayer), config->radius) || !agreement) {
                    logger::info("PersonalStories: {} decision lacks a fresh actual directed player quote", id); return;
                }
            }
            Story changed = *story;
            if (decision == "propose") {
                if (changed.phase != Phase::Proposed) return;
                const auto reason = parameters.value("motivation", changed.reason);
                if (!ValidText(reason, MAX_REASON_BYTES) || reason.empty()) return;
                changed.reason = reason;
                ++changed.revision;
            }
            if (changed.phase == Phase::Proposed && (decision == "propose" || decision == "accept")) {
                const auto requested = parameters.value("destination_id", std::string{});
                if (!requested.empty()) {
                    auto found = std::find_if(config->destinations.begin(), config->destinations.end(), [&](const auto& destination) { return destination.id == requested; });
                    if (found == config->destinations.end() || !found->resolved || (changed.kind == Kind::Expedition && Cleared(found->resolved))) return;
                    if (changed.destination != found->resolved) { changed.destination = found->resolved; ++changed.revision; }
                }
                if (decision == "accept" && changed.kind == Kind::Expedition && Cleared(changed.destination)) return;
            }
            const auto now = Now();
            Result result = Result::Applied;
            if (decision == "accept") result = Accept(changed, changed.revision, now);
            else if (decision == "pause") result = Pause(changed, changed.revision);
            else if (decision == "resume") result = Resume(changed, changed.revision);
            else if (decision == "decline") result = Decline(changed, changed.revision);
            if (result != Result::Applied) return;
            if (changed.kind == Kind::Gathering && (decision == "accept" || decision == "resume")) {
                const auto hour = parameters.value("meeting_hour", config->gatheringHour);
                const auto dayOffset = parameters.value("meeting_day_offset", 0);
                if (!std::isfinite(hour) || hour < 0 || hour >= 24 || dayOffset < 0 || dayOffset > 7) return;
                float appointment = std::floor(now) + static_cast<float>(dayOffset) + hour / 24.0f;
                if (appointment < now) appointment += 1.0f;
                Defer(changed, changed.revision, appointment);
            }
            if (!Valid(changed)) return;
            *story = std::move(changed);
            if (agreement) g_decisionCutoff[id] = agreement->serial;
            g_exchanges.erase(id);
            g_clearBaseline[id] = Cleared(story->destination);
            logger::info("PersonalStories: {} decision {} phase {} revision {}", id, decision, static_cast<int>(story->phase), story->revision);
            Publish();
        }
        bool QueueAction(RE::Actor* opaqueActor, std::string arguments, bool reflection) {
            try {
            const auto epoch = AsyncDispatch::CurrentSessionEpoch();
            if (!epoch || arguments.size() > 8192) return false;
            RE::FormID actorID = 0;
            {
                std::lock_guard lock(g_cacheMutex);
                const auto found = g_cache.find(opaqueActor);
                if (found == g_cache.end() || !(reflection ? found->second.canReflect : found->second.canPlan)) return false;
                actorID = found->second.form;
            }
            auto parameters = Json::parse(arguments, nullptr, false);
            if (!parameters.is_object() || parameters.value("session_epoch", std::uint64_t{0}) != epoch) return false;
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) return false;
            tasks->AddTask([epoch, actorID, opaqueActor, parameters = std::move(parameters), reflection]() {
                if (!AsyncDispatch::IsSessionCurrent(epoch) || Actor(actorID) != opaqueActor) return;
                try { ApplyAction(actorID, parameters, reflection); }
                catch (const std::exception& error) { logger::warn("PersonalStories action rejected: {}", error.what()); }
            });
            return true;
            } catch (const std::exception& error) {
                logger::warn("PersonalStories action input rejected: {}", error.what());
                return false;
            }
        }
    }

    void Initialize() {
        ReadConfig();
        if (g_registered) return;
        if (!SkyrimNetAPI::RegisterDecorator || !SkyrimNetAPI::RegisterCPPAction || !SkyrimNetAPI::RegisterEventCallback || !SkyrimNetAPI::UnregisterEventCallback) {
            logger::error("PersonalStories: required SkyrimNet action/decorator/event API unavailable"); return;
        }
        const bool decorator = SkyrimNetAPI::RegisterDecorator("companion_story_context", "Actor-scoped personal plans and engine-observed milestones.",
            [](RE::Actor* opaqueActor) {
                std::lock_guard lock(g_cacheMutex);
                const auto found = g_cache.find(opaqueActor);
                return found == g_cache.end() ? std::string{} : found->second.context;
            });
        auto eligible = [](RE::Actor* opaqueActor, bool reflection) {
            std::lock_guard lock(g_cacheMutex);
            const auto found = g_cache.find(opaqueActor);
            return found != g_cache.end() && (reflection ? found->second.canReflect : found->second.canPlan);
        };
        const std::string planSchema = R"({"type":"object","properties":{"session_epoch":{"type":"integer"},"story_id":{"type":"string"},"revision":{"type":"integer"},"decision":{"type":"string","enum":["propose","accept","pause","resume","decline"]},"motivation":{"type":"string"},"destination_id":{"type":"string"},"agreement_event_id":{"type":"integer"},"agreement_quote":{"type":"string"},"meeting_hour":{"type":"number"},"meeting_day_offset":{"type":"integer"}},"required":["session_epoch","story_id","revision","decision"]})";
        const bool plan = SkyrimNetAPI::RegisterCPPAction("CompanionStoryPlan",
            "Manage your own concrete personal project from companion_story_context. Copy its session_epoch, story_id and revision exactly. Propose a motivation/destination; accept, pause, resume or decline ONLY when an actual recent player line unambiguously agrees to that exact decision. Copy its observed agreement_event_id and full agreement_quote. Silence, speculation, an unrelated yes, and your own narration are not agreement. Expedition destination_id must be an offered choice. For a gathering supply meeting_hour and meeting_day_offset from the agreed appointment. This records a plan and never proves an objective happened.",
            [eligible](RE::Actor* actor) { return eligible(actor, false); },
            [](RE::Actor* actor, std::string arguments) { return QueueAction(actor, std::move(arguments), false); },
            "dialogue,dialogue_player_text,dialogue_player_stt", "Social", 50, planSchema, "", "", "");
        const bool reflection = SkyrimNetAPI::RegisterCPPAction("CompanionStoryReflect",
            "Copy the current session_epoch, story_id and revision. Finish your own project only when its state is observed outcome; reflection pending. First speak naturally about what happened and what you want next. Then copy the exact later owner dialogue event_id and full line as reflection_event_id/reflection_quote. The engine records that actual line as the durable aftermath. Never claim a milestone, quote an invented line, or use the reunion's completion line as its later reflection.",
            [eligible](RE::Actor* actor) { return eligible(actor, true); },
            [](RE::Actor* actor, std::string arguments) { return QueueAction(actor, std::move(arguments), true); },
            "dialogue", "Social", 50,
            R"({"type":"object","properties":{"session_epoch":{"type":"integer"},"story_id":{"type":"string"},"revision":{"type":"integer"},"reflection_event_id":{"type":"integer"},"reflection_quote":{"type":"string"}},"required":["session_epoch","story_id","revision","reflection_event_id","reflection_quote"]})", "", "", "");
        g_registered = decorator && plan && reflection;
        logger::info("PersonalStories: native integration registered={}", g_registered);
    }

    static void UnregisterDialogueCallbacks() {
        if (SkyrimNetAPI::UnregisterEventCallback)
            for (auto id : g_eventCallbacks) SkyrimNetAPI::UnregisterEventCallback(id);
        g_eventCallbacks.clear();
    }
    static void RegisterDialogueCallbacks(std::uint64_t epoch) {
        UnregisterDialogueCallbacks();
        for (const char* type : {"dialogue", "dialogue_player_text", "dialogue_player_stt", "dialogue_player", "dialogue_npc"}) {
            auto id = SkyrimNetAPI::RegisterEventCallback(type, [epoch, sourceType = std::string(type)](const char* payload) {
                if (!AsyncDispatch::IsSessionCurrent(epoch)) return;
                try {
                    auto event = ParseDialogue(payload);
                    auto* tasks = SKSE::GetTaskInterface();
                    if (!event || !tasks) return;
                    event->sourceType = sourceType;
                    tasks->AddTask([epoch, event = std::move(*event)]() mutable {
                        if (!AsyncDispatch::IsSessionCurrent(epoch)) return;
                        try { OnDialogueMain(std::move(event)); }
                        catch (const std::exception& error) { logger::warn("PersonalStories dialogue rejected: {}", error.what()); }
                    });
                } catch (const std::exception& error) { logger::debug("PersonalStories ignored dialogue payload: {}", error.what()); }
            });
            if (id) g_eventCallbacks.push_back(id);
        }
        if (g_eventCallbacks.size() != 5) {
            UnregisterDialogueCallbacks();
            g_ready = false;
            logger::error("PersonalStories: incomplete dialogue callback registration; session inactive");
        }
    }

    void ResumeSession() {
        g_ready = g_registered && !g_blocked && AsyncDispatch::CurrentSessionEpoch() != 0;
        if (!g_ready) { Publish(); return; }
        RegisterDialogueCallbacks(AsyncDispatch::CurrentSessionEpoch());
        if (!g_ready) { Publish(); return; }
        const auto now = Now();
        for (auto& config : g_configs) {
            if (!ResolveConfig(config)) continue;
            auto* saved = Find(g_store, config.id);
            if (!saved) {
                Story story;
                story.id = config.id; story.kind = config.kind; story.owner = config.ownerID;
                story.target = config.targetID; story.participants = config.participantIDs;
                story.offeredAt = now; story.reason = config.reason;
                if (config.destinations.size() == 1) story.destination = config.destinations.front().resolved;
                Offer(g_store, std::move(story));
                saved = Find(g_store, config.id);
            }
            if (!saved || !Matches(*saved, config)) logger::warn("PersonalStories: {} saved identities differ from configuration; retained inactive", config.id);
            if (saved) {
                g_clearBaseline[saved->id] = Cleared(saved->destination);
                for (const auto evidence : saved->evidenceIds) if (evidence >= kEngineEvidence) g_nextEvidence = std::max(g_nextEvidence, evidence);
            }
        }
        Publish();
    }

    void Tick() {
        if (!g_ready || g_blocked || !AsyncDispatch::CurrentSessionEpoch()) return;
        auto* ui = RE::UI::GetSingleton();
        if (ui && ui->GameIsPaused()) return;
        const auto clock = std::chrono::steady_clock::now();
        if (clock - g_lastTick < std::chrono::seconds(1)) return;
        g_lastTick = clock;
        const auto now = Now();
        for (auto& story : g_store.stories) {
            auto* config = Configuration(story.id);
            if (!config || !Matches(story, *config)) continue;
            auto* owner = Actor(story.owner);
            auto* target = Actor(story.target);
            if (owner && owner->IsDead()) { Fail(story, story.revision, Failure::OwnerDied); continue; }
            if (target && target->IsDead()) { Fail(story, story.revision, Failure::TargetDied); continue; }
            if (story.phase != Phase::Active) continue;
            if (story.kind == Kind::Expedition) {
                const bool cleared = Cleared(story.destination);
                const bool together = Together(story, *config, true);
                if (together) {
                    ObserveFact(story, Observation::EnteredTogether, now);
                    if (cleared && !g_clearBaseline[story.id]) ObserveFact(story, Observation::ClearedTogether, now);
                } else if (Has(story, Observation::EnteredTogether) && Together(story, *config, false)) {
                    bool outside = !AtDestination(owner, story.destination, false);
                    for (auto participant : story.participants) outside &= !AtDestination(Actor(participant), story.destination, false);
                    if (outside) ObserveFact(story, Observation::ExitedTogether, now);
                }
                g_clearBaseline[story.id] = cleared;
            } else {
                if (story.kind == Kind::Gathering && HasSupplies(story, *config)) ObserveFact(story, Observation::SuppliesObserved, now);
                if (SocialReady(story, *config, now)) ObserveFact(story, Observation::RequiredParticipantsPresent, now);
                else g_exchanges.erase(story.id);
            }
        }
        Publish();
    }

    void Revert() {
        g_ready = false; g_blocked = false;
        UnregisterDialogueCallbacks();
        g_store.stories.clear(); g_records.clear(); g_dialogue.clear();
        g_recordBytes = 0; g_recordCount = 0;
        g_exchanges.clear(); g_clearBaseline.clear(); g_nextEvidence = kEngineEvidence;
        g_reflectionCutoff.clear(); g_decisionCutoff.clear(); g_dialogueSerial = 0;
        std::lock_guard lock(g_cacheMutex);
        g_cache.clear();
    }

    void Load(SKSE::SerializationInterface* serialization, std::uint32_t version, std::uint32_t length) {
        if (!serialization) return;
        if (g_recordCount >= kMaximumOpaqueRecords || length > kMaximumOpaqueBytes - g_recordBytes) {
            g_blocked = true;
            g_store.stories.clear();
            logger::error("PersonalStories: serialization record count/size limit exceeded; state quarantined inactive");
            return;
        }
        ++g_recordCount;
        g_recordBytes += length;
        RawRecord record{version, std::vector<unsigned char>(length)};
        if (serialization->ReadRecordData(record.bytes.data(), length) != length) {
            g_blocked = true; g_store.stories.clear();
            logger::error("PersonalStories: truncated serialization record; feature disabled"); return;
        }
        g_records.push_back(record);
        Store restored;
        if (g_blocked || g_recordCount != 1 || !Decode(record.bytes, version, restored) ||
            !RemapForms(restored, [&](std::uint32_t oldID, std::uint32_t& current) { return serialization->ResolveFormID(oldID, current); })) {
            g_blocked = true; g_store.stories.clear();
            logger::error("PersonalStories: unknown, duplicate, malformed or unresolved state quarantined as inert diagnostic bytes"); return;
        }
        g_store = std::move(restored);
    }

    void Save(SKSE::SerializationInterface* serialization) {
        if (!serialization) return;
        if (g_blocked) {
            // Version zero is permanently inert. Unremapped IDs cannot be
            // re-emitted as readable state under a later save's form mapping.
            if (g_records.empty()) {
                if (!serialization->OpenRecord(kRecordType, 0)) logger::error("PersonalStories: failed to write quarantine marker");
            }
            for (const auto& record : g_records) {
                if (!serialization->OpenRecord(kRecordType, 0) ||
                    (!record.bytes.empty() && !serialization->WriteRecordData(record.bytes.data(), static_cast<std::uint32_t>(record.bytes.size()))))
                    logger::error("PersonalStories: failed to write inert diagnostic record");
            }
            return;
        }
        std::vector<unsigned char> bytes;
        if (!Encode(g_store, bytes)) { logger::error("PersonalStories: refused invalid state serialization"); return; }
        if (!serialization->OpenRecord(kRecordType, kRecordVersion) ||
            !serialization->WriteRecordData(bytes.data(), static_cast<std::uint32_t>(bytes.size())))
            logger::error("PersonalStories: failed to write state");
    }
}
