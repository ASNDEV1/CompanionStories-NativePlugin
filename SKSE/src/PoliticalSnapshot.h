#pragma once

// SQLite image codec shared by the runtime and the focused persistence test.
// No game objects, callbacks, or external writes are involved here.
#include <sqlite3.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace IntelEngine::Persistence {
    inline constexpr uint32_t MAX_POLITICAL_BYTES = 64 * 1024 * 1024;
    struct CloseDatabase {
        void operator()(sqlite3* db) const { if (db) sqlite3_close(db); }
    };
    using Database = std::unique_ptr<sqlite3, CloseDatabase>;

    inline Database OpenMemoryDatabase() {
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open_v2(":memory:", &raw,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        Database db(raw);
        if (rc != SQLITE_OK) return {};
        return db;
    }

    inline bool CopyDatabase(sqlite3* destination, sqlite3* source) {
        auto* transfer = sqlite3_backup_init(destination, "main", source, "main");
        if (!transfer) return false;
        const int step = sqlite3_backup_step(transfer, -1);
        const int finish = sqlite3_backup_finish(transfer);
        return step == SQLITE_DONE && finish == SQLITE_OK;
    }

    inline bool EncodePoliticalSnapshot(sqlite3* db, std::vector<unsigned char>& result) {
        sqlite3_int64 size = 0;
        sqlite3_serialize(db, "main", &size, SQLITE_SERIALIZE_NOCOPY);
        if (size < 100 || size > MAX_POLITICAL_BYTES) return false;
        auto* bytes = sqlite3_serialize(db, "main", &size, 0);
        if (!bytes) return false;
        const bool valid = size >= 100 && size <= MAX_POLITICAL_BYTES;
        if (valid) result.assign(bytes, bytes + size);
        sqlite3_free(bytes);
        return valid;
    }

    inline Database DecodePoliticalSnapshot(std::span<const unsigned char> bytes) {
        if (bytes.size() < 100 || bytes.size() > MAX_POLITICAL_BYTES) return {};
        auto source = OpenMemoryDatabase();
        if (!source) return {};
        // READONLY makes SQLite borrow this buffer; it never frees or resizes it.
        // The copy completes and source closes while the caller's bytes are alive.
        if (sqlite3_deserialize(source.get(), "main", const_cast<unsigned char*>(bytes.data()),
                bytes.size(), bytes.size(), SQLITE_DESERIALIZE_READONLY) != SQLITE_OK) return {};
        sqlite3_stmt* check = nullptr;
        if (sqlite3_prepare_v2(source.get(), "PRAGMA quick_check(1)", -1, &check, nullptr) != SQLITE_OK)
            return {};
        const int step = sqlite3_step(check);
        const auto* status = step == SQLITE_ROW ? sqlite3_column_text(check, 0) : nullptr;
        const bool healthy = status && std::string_view(reinterpret_cast<const char*>(status)) == "ok";
        sqlite3_finalize(check);
        if (!healthy) return {};
        // Validate the runtime schema before publishing the database. Missing or
        // incompatible tables must not quietly become a fresh empty campaign.
        constexpr const char* queries[] = {
            "SELECT id,faction_a,faction_b,relation_score,trade_active,war_active,war_start_time,last_event_time FROM faction_relations LIMIT 0",
            "SELECT id,faction_a,faction_b,event_type,description,relation_delta,game_time,instigator_npc FROM faction_events LIMIT 0",
            "SELECT faction_id,standing,title,last_change_time,total_contributions FROM player_faction_standing LIMIT 0",
            "SELECT id,faction_id,delta,reason,game_time FROM player_standing_history LIMIT 0",
            "SELECT id,faction_a,faction_b,start_time,end_time,victor,battles_fought,faction_a_morale,faction_b_morale,faction_a_strength,faction_b_strength FROM faction_wars LIMIT 0",
            "SELECT id,war_id,location_name,game_time,attacker,defender,result,attacker_losses,defender_losses,player_participated,player_side,narrative FROM war_battles LIMIT 0"
        };
        for (const auto* query : queries) {
            sqlite3_stmt* statement = nullptr;
            const int rc = sqlite3_prepare_v2(source.get(), query, -1, &statement, nullptr);
            sqlite3_finalize(statement);
            if (rc != SQLITE_OK) return {};
        }
        auto result = OpenMemoryDatabase();
        if (!result || !CopyDatabase(result.get(), source.get())) return {};
        if (sqlite3_exec(result.get(), "PRAGMA journal_mode=MEMORY; PRAGMA foreign_keys=ON",
                nullptr, nullptr, nullptr) != SQLITE_OK) return {};
        return result;
    }
}
