#include "../SKSE/src/PoliticalSnapshot.h"
#include "../SKSE/src/SlotRecord.h"
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace IntelEngine::Persistence;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Sql(sqlite3* db, const char* sql) { Require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, sql); }
int Number(sqlite3* db, const char* query) {
    sqlite3_stmt* s = nullptr;
    Require(sqlite3_prepare_v2(db, query, -1, &s, nullptr) == SQLITE_OK, query);
    Require(sqlite3_step(s) == SQLITE_ROW, query);
    int result = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return result;
}
const char* schema = R"SQL(
            CREATE TABLE IF NOT EXISTS faction_relations (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                faction_a TEXT NOT NULL,
                faction_b TEXT NOT NULL,
                relation_score INTEGER DEFAULT 0,
                trade_active BOOLEAN DEFAULT 0,
                war_active BOOLEAN DEFAULT 0,
                war_start_time REAL,
                last_event_time REAL,
                UNIQUE(faction_a, faction_b)
            );

            CREATE TABLE IF NOT EXISTS faction_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                faction_a TEXT NOT NULL,
                faction_b TEXT,
                event_type TEXT NOT NULL,
                description TEXT NOT NULL,
                relation_delta INTEGER DEFAULT 0,
                game_time REAL,
                instigator_npc TEXT
            );

            CREATE TABLE IF NOT EXISTS player_faction_standing (
                faction_id TEXT PRIMARY KEY,
                standing INTEGER DEFAULT 0,
                title TEXT,
                last_change_time REAL,
                total_contributions INTEGER DEFAULT 0
            );

            CREATE TABLE IF NOT EXISTS player_standing_history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                faction_id TEXT NOT NULL,
                delta INTEGER NOT NULL,
                reason TEXT,
                game_time REAL NOT NULL
            );

            CREATE TABLE IF NOT EXISTS faction_wars (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                faction_a TEXT NOT NULL,
                faction_b TEXT NOT NULL,
                start_time REAL NOT NULL,
                end_time REAL,
                victor TEXT,
                battles_fought INTEGER DEFAULT 0,
                faction_a_morale INTEGER DEFAULT 100,
                faction_b_morale INTEGER DEFAULT 100,
                faction_a_strength INTEGER DEFAULT 100,
                faction_b_strength INTEGER DEFAULT 100,
                UNIQUE(faction_a, faction_b, start_time)
            );

            CREATE TABLE IF NOT EXISTS war_battles (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                war_id INTEGER NOT NULL,
                location_name TEXT NOT NULL,
                game_time REAL,
                attacker TEXT NOT NULL,
                defender TEXT NOT NULL,
                result TEXT,
                attacker_losses INTEGER DEFAULT 0,
                defender_losses INTEGER DEFAULT 0,
                player_participated BOOLEAN DEFAULT 0,
                player_side TEXT,
                narrative TEXT,
                FOREIGN KEY (war_id) REFERENCES faction_wars(id)
            );

            CREATE INDEX IF NOT EXISTS idx_events_game_time ON faction_events(game_time DESC);
            CREATE INDEX IF NOT EXISTS idx_events_faction ON faction_events(faction_a, faction_b);
            CREATE INDEX IF NOT EXISTS idx_wars_active ON faction_wars(end_time) WHERE end_time IS NULL;
            CREATE INDEX IF NOT EXISTS idx_battles_war ON war_battles(war_id);
        )SQL";
void PoliticalTests() {
    auto source = OpenMemoryDatabase();
    Require(bool(source), "open memory");
    Sql(source.get(), schema);
    Sql(source.get(), "INSERT INTO faction_events(faction_a,event_type,description,game_time) VALUES('A','visit','Lydia arrived',1);");
    Sql(source.get(), "INSERT INTO player_faction_standing VALUES('A',7,'Friend',1,12);");
    Sql(source.get(), "INSERT INTO faction_wars(faction_a,faction_b,start_time,faction_a_morale) VALUES('A','B',1,73);");
    std::vector<unsigned char> early, later;
    Require(EncodePoliticalSnapshot(source.get(), early), "encode early");
    Sql(source.get(), "INSERT INTO faction_events(faction_a,event_type,description,game_time) VALUES('A','reunion','Arinel met Aelanye',2);");
    Sql(source.get(), "UPDATE player_faction_standing SET standing=23,last_change_time=2;");
    Sql(source.get(), "UPDATE faction_wars SET faction_a_morale=31,battles_fought=2;");
    Require(EncodePoliticalSnapshot(source.get(), later), "encode later");
    auto a = DecodePoliticalSnapshot(early);
    Require(bool(a), "decode early");
    Require(Number(a.get(), "SELECT count(*) FROM faction_events") == 1, "old save isolates events");
    Require(Number(a.get(), "SELECT standing FROM player_faction_standing") == 7, "old save isolates standing");
    Sql(a.get(), "DELETE FROM faction_events; UPDATE player_faction_standing SET standing=99;");
    auto b = DecodePoliticalSnapshot(later);
    Require(bool(b), "decode later after older timeline mutation");
    Require(Number(b.get(), "SELECT count(*) FROM faction_events") == 2, "newer save history survives");
    Require(Number(b.get(), "SELECT standing FROM player_faction_standing") == 23, "newer save standing survives");
    Require(Number(b.get(), "SELECT faction_a_morale FROM faction_wars") == 31, "war state snapshots exactly");
    Require(Number(b.get(), "SELECT total_contributions FROM player_faction_standing") == 12, "legacy columns preserved");
    Sql(b.get(), "INSERT INTO faction_events(faction_a,event_type,description) VALUES('A','next','next');");
    Require(Number(b.get(), "SELECT max(id) FROM faction_events") == 3, "autoincrement identity preserved");
    Sql(source.get(), "PRAGMA query_only=ON;");
    auto imported = OpenMemoryDatabase();
    Require(CopyDatabase(imported.get(), source.get()), "readonly legacy import");
    Sql(imported.get(), "DELETE FROM faction_events WHERE game_time > 1;");
    Require(Number(source.get(), "SELECT count(*) FROM faction_events") == 2, "legacy import leaves source unchanged");
    Require(Number(imported.get(), "SELECT count(*) FROM faction_events") == 1, "legacy projection local only");
    std::vector<unsigned char> garbage(200, 0xFF);
    Require(!DecodePoliticalSnapshot(garbage), "reject invalid SQLite image");
    Require(!DecodePoliticalSnapshot(std::span(early).first(early.size()/2)), "reject truncated SQLite image");
    auto wrong = OpenMemoryDatabase();
    Sql(wrong.get(), "CREATE TABLE unrelated(x);");
    Require(EncodePoliticalSnapshot(wrong.get(), garbage), "encode schema fixture");
    Require(!DecodePoliticalSnapshot(garbage), "reject incompatible political schema");
}
void SlotTests() {
    // Legacy v1 fixture assembled independently of the writer under test.
    std::vector<unsigned char> legacy;
    auto number = [&](uint32_t n) { for (int i=0;i<4;++i) legacy.push_back((n >> (i*8)) & 255); };
    auto string = [&](const std::string& s) { number(static_cast<uint32_t>(s.size())); legacy.insert(legacy.end(),s.begin(),s.end()); };
    for (int i=0;i<5;++i) {
        number(i==0 ? 0x01000D62 : 0); number(i==0 ? 1 : 0);
        string(i==0 ? "travel" : ""); string(i==0 ? "High Hrothgar" : "");
        number(i==0 ? 2 : 0); number(std::bit_cast<uint32_t>(i==0 ? 17.5f : 0.0f)); number(0);
    }
    std::array<SavedSlot,SLOT_COUNT> decoded;
    Require(DecodeSlots(legacy,1,decoded), "reads legacy v1");
    Require(decoded[0].formID==0x01000D62 && decoded[0].deadline==17.5f && decoded[0].targetName=="High Hrothgar", "legacy values preserved for remap");
    for (size_t size=0;size<legacy.size();++size) {
        std::array<SavedSlot,SLOT_COUNT> sentinel;
        sentinel[0].targetName="unchanged";
        Require(!DecodeSlots(std::span(legacy).first(size),1,sentinel), "reject every truncated prefix");
        Require(sentinel[0].targetName=="unchanged", "failed decode publishes no partial state");
    }
    Require(!DecodeSlots(legacy,2,decoded), "unknown record version rejected");
    auto damaged=legacy; damaged.push_back(0);
    Require(!DecodeSlots(damaged,1,decoded), "unexpected trailing layout rejected");
    damaged=legacy; for(size_t i=8;i<12;++i) damaged[i]=255;
    Require(!DecodeSlots(damaged,1,decoded), "unbounded string length rejected");
    Require(DecodeSlots(legacy,1,decoded), "restore fixture");
    decoded[0].deadline=std::numeric_limits<float>::quiet_NaN();
    Require(!EncodeSlots(decoded,damaged), "NaN deadline rejected");
    decoded[0].deadline=7.0f; decoded[0].targetName=std::string(1024,'x');
    Require(EncodeSlots(decoded,damaged), "bounded long names supported");
    Require(DecodeSlots(damaged,1,decoded) && decoded[0].targetName.size()==1024, "long name roundtrip");
}
int main() {
    try { PoliticalTests(); SlotTests(); std::cout << "Political snapshot timelines, readonly migration, malformed images, legacy IETK, atomic truncation and bounds: passed\n"; }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
