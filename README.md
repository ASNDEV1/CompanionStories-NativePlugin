# Companion Stories

A companion-focused fork of Galanx's IntelEngine. This project is independent of Galanx and is not endorsed by the upstream developer.

The active feature is a small, evidence-driven personal-story engine for SkyrimNet. Models propose motives and reflect on events. Native observations establish whether configured people actually travelled together, attended a gathering, or exchanged dialogue. Plans and aftermath belong to each save.

Legacy autonomous politics, wars, ambushes, stalking, generic quest generation, gossip and automatic biography rewriting are retired. Ordinary follower, inventory, employment and travel actions stay with the user's existing action stack. Compatibility readers and owned-state cleanup remain for old IntelEngine saves.

Runtime filenames `IntelEngine.dll`, `IntelEngine.esp`, existing Papyrus names and the IEPS serialization owner are preserved for existing saves. The project/package title is Companion Stories. Apache-2.0 LICENSE and NOTICE are retained. See `docs/COMPANION-STORIES.md` for the changed architecture and limits.

Build the SKSE target with CMake, CommonLibSSE-NG, fmt, spdlog, nlohmann-json, rapidfuzz and sqlite3. `COMMONLIBSSE_NG_ROOT` can point at a CommonLib source checkout. The output uses the dynamic MSVC runtime to match SkyrimNet's C++ ABI. Automatic deployment is disabled by default. Compile all Papyrus sources with the Skyrim SE Creation Kit compiler and the actual SKSE, SkyUI and declared mod API sources. The companion game fork supplies the unchanged ESP and its quest fragment.

Actor and location definitions are loaded from `Data/SKSE/Plugins/CompanionStories.json`. No private campaign actors or biographies are bundled here. The engine registers CompanionStoryPlan, CompanionStoryReflect and companion_story_context. A character-bio prompt should call that decorator for the current actor; action parameters must quote actual recent dialogue evidence and the current session/revision.

Tests in `tests/personal-story-state.cpp` exercise deterministic story transitions and serialization without Skyrim. `tests/persistence.cpp` exercises SQLite snapshots and legacy task decoding. These do not replace in-game testing of the SkyrimNet adapter and existing-save cleanup.
