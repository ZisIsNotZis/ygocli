// Yu-Gi-Oh! Minimal CLI - Complete version
// Usage: ./ygo-mincli <deck0.ydk> <deck1.ydk> [--auto] [--mcp]
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <thread>
#include <chrono>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <cstdlib>
#include <array>
#include <iomanip>
#include <algorithm>
#include <limits>
#include <deque>
#include <cctype>
#include <unistd.h>
#include <climits>
#include <glob.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <poll.h>

// JSON (nlohmann/json)
#include <nlohmann/json.hpp>

// SQLite3
#include <sqlite3.h>

// BufferIO implementation
class BufferIO {
public:
    template<typename T>
    static T Read(unsigned char*& p) {
        T ret{};
        std::memcpy(&ret, p, sizeof(T));
        p += sizeof(T);
        return ret;
    }
    template<typename T>
    static void Write(unsigned char*& p, const T& value) {
        std::memcpy(p, &value, sizeof(T));
        p += sizeof(T);
    }
};

// ocgcore headers
#define OCGCOREAPI
#include "ocgcore/ocgapi.h"
#include "ocgcore/common.h"
#include "ocgcore/card_data.h"

// gframe-compatible network protocol (vendored, byte-exact)
#include "network.h"
#include "net.h"

// gframe structs (HostInfo, STOC_*, CTOS_*) live in namespace ygo
using namespace ygo;

// gframe protocol version (config.h)
constexpr uint16_t PRO_VERSION = 0x1362;

// Card database
std::unordered_map<uint32_t, std::string> card_names;
std::unordered_map<uint32_t, std::string> card_descs;
std::unordered_map<uint32_t, card_data> card_datas;
std::unordered_map<uint32_t, std::string> sys_strings;
// Per-card description offsets (str1..str16) for resolving desc codes like
// (card_code << 4) | offset that scripts pass via Auxiliary.Stringid().
std::unordered_map<uint32_t, std::array<std::string, 16>> card_strs;
sqlite3* db = nullptr;

void load_card_database(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        return;
    }

    sqlite3_stmt* stmt;
    std::string sql = "SELECT id, name, desc, str1, str2, str3, str4, str5, str6, str7, str8, str9, str10, str11, str12, str13, str14, str15, str16 FROM texts";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            uint32_t id = sqlite3_column_int(stmt, 0);
            const unsigned char* name = sqlite3_column_text(stmt, 1);
            const unsigned char* desc = sqlite3_column_text(stmt, 2);
            if (name) {
                card_names[id] = reinterpret_cast<const char*>(name);
            }
            if (desc) {
                card_descs[id] = reinterpret_cast<const char*>(desc);
            }
            std::array<std::string, 16>& strs = card_strs[id];
            for (int i = 0; i < 16; i++) {
                const unsigned char* s = sqlite3_column_text(stmt, 3 + i);
                if (s) strs[i] = reinterpret_cast<const char*>(s);
            }
        }
        sqlite3_finalize(stmt);
    }

    sql = "SELECT id, ot, alias, setcode, type, atk, def, level, race, attribute FROM datas";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            card_data data;
            data.clear();
            data.code = sqlite3_column_int(stmt, 0);
            data.alias = sqlite3_column_int(stmt, 2);
            uint64_t setcode_val = sqlite3_column_int64(stmt, 3);
            write_setcode(data.setcode, setcode_val);
            data.type = sqlite3_column_int(stmt, 4);
            data.attack = sqlite3_column_int(stmt, 5);
            data.defense = sqlite3_column_int(stmt, 6);
            data.level = sqlite3_column_int(stmt, 7);
            data.race = sqlite3_column_int(stmt, 8);
            data.attribute = sqlite3_column_int(stmt, 9);
            card_datas[data.code] = data;
        }
        sqlite3_finalize(stmt);
    }
}

void load_strings_conf(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.rfind("!system", 0) != 0) {
            continue;
        }

        std::istringstream iss(line);
        std::string tag;
        std::string id_text;
        if (!(iss >> tag >> id_text)) {
            continue;
        }

        std::string value;
        std::getline(iss, value);
        auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        value.erase(0, first);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
            value.pop_back();
        }

        uint32_t id = static_cast<uint32_t>(std::strtoul(id_text.c_str(), nullptr, 0));
        sys_strings[id] = value;
    }
}

std::string get_card_name(uint32_t id) {
    if (id == 0) return "None";
    if (card_names.count(id)) return card_names[id];
    return "Card-" + std::to_string(id);
}

std::string get_sys_string(uint32_t id) {
    auto it = sys_strings.find(id);
    if (it != sys_strings.end()) {
        return it->second;
    }
    return "Sys-" + std::to_string(id);
}

// Resolve a description code the way the GUI DataManager::GetDesc() does.
// strCode <= 0x7ff is a system string (strings.conf); otherwise it's
// (card_code << 4) | offset, looking up the card's str1..str16 column.
constexpr uint32_t MAX_STRING_ID = 0x7ff;
std::string get_desc_string(uint32_t strCode) {
    if (strCode <= MAX_STRING_ID) {
        return get_sys_string(strCode);
    }
    uint32_t code = (strCode >> 4) & 0x0fffffff;
    uint32_t offset = strCode & 0xf;
    auto it = card_strs.find(code);
    if (it == card_strs.end()) return "Desc-" + std::to_string(strCode);
    if (!it->second[offset].empty()) return it->second[offset];
    return "Desc-" + std::to_string(strCode);
}

std::string normalize_effect_text(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        if (c == '\r' || c == '\n') {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            while (i + 1 < src.size() && (src[i + 1] == '\r' || src[i + 1] == '\n')) {
                ++i;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string location_name(uint32_t loc) {
    // Overlay is OR'd with the host's location (e.g. MZone|Overlay, Extra|Overlay).
    if (loc & LOCATION_OVERLAY) return "Overlay";
    switch (loc) {
        case LOCATION_DECK: return "Deck";
        case LOCATION_HAND: return "Hand";
        case LOCATION_MZONE: return "MZone";
        case LOCATION_SZONE: return "SZone";
        case LOCATION_GRAVE: return "Grave";
        case LOCATION_REMOVED: return "Removed";
        case LOCATION_EXTRA: return "Extra";
        case LOCATION_FZONE: return "FZone";
        case LOCATION_PZONE: return "PZone";
        default: return "Loc-" + std::to_string(loc);
    }
}

std::string pos_name(int pos) {
    switch (pos) {
        case POS_FACEUP_ATTACK: return "FA";
        case POS_FACEDOWN_ATTACK: return "DA";
        case POS_FACEUP_DEFENSE: return "FD";
        case POS_FACEDOWN_DEFENSE: return "DD";
        case POS_FACEUP: return "UP";       // S/T face-up (0x5)
        case POS_FACEDOWN: return "FD";     // S/T face-down (0xa)
        default: return "?";
    }
}

// S/T position label: face-up / face-down.
// Set S/T cards use POS_FACEDOWN (0xa), not POS_FACEDOWN_ATTACK (0x2).
std::string st_pos_label(int pos) {
    switch (pos) {
        case POS_FACEUP: return "UP";       // 0x5
        case POS_FACEDOWN: return "FD";     // 0xa
        case POS_FACEUP_ATTACK: return "UP";
        case POS_FACEDOWN_ATTACK: return "FD";
        default: return "?";
    }
}

// Level/rank label for a field monster: "Lv4" / "R3" (Xyz).
// get_level() returns 0 for Xyz/Link monsters, so rank must be passed for Xyz.
std::string mcp_lv_label(uint32_t code, uint32_t level, uint32_t rank) {
    (void)code;  // code is not needed; level/rank come from the query
    if (rank) return " R" + std::to_string(rank & 0xff);
    return " Lv" + std::to_string(level & 0xff);
}

const char* phase_name(int phase) {
    switch (phase) {
        case 0x01: return "Draw";
        case 0x02: return "Standby";
        case 0x04: return "Main1";
        case 0x08: return "Battle_Start";
        case 0x10: return "Battle_Step";
        case 0x20: return "Damage";
        case 0x40: return "Damage_Cal";
        case 0x80: return "Battle";
        case 0x100: return "Main2";
        case 0x200: return "End";
        default: return "Unknown";
    }
}

const char* msg_type_name(int type) {
    switch (type) {
        case MSG_RETRY: return "MSG_RETRY";
        case MSG_WIN: return "MSG_WIN";
        case MSG_NEW_TURN: return "MSG_NEW_TURN";
        case MSG_NEW_PHASE: return "MSG_NEW_PHASE";
        case MSG_MOVE: return "MSG_MOVE";
        case MSG_POS_CHANGE: return "MSG_POS_CHANGE";
        case MSG_SET: return "MSG_SET";
        case MSG_SWAP: return "MSG_SWAP";
        case MSG_FIELD_DISABLED: return "MSG_FIELD_DISABLED";
        case MSG_SUMMONING: return "MSG_SUMMONING";
        case MSG_SUMMONED: return "MSG_SUMMONED";
        case MSG_SPSUMMONING: return "MSG_SPSUMMONING";
        case MSG_SPSUMMONED: return "MSG_SPSUMMONED";
        case MSG_FLIPSUMMONING: return "MSG_FLIPSUMMONING";
        case MSG_FLIPSUMMONED: return "MSG_FLIPSUMMONED";
        case MSG_CHAINING: return "MSG_CHAINING";
        case MSG_CHAINED: return "MSG_CHAINED";
        case MSG_CHAIN_SOLVING: return "MSG_CHAIN_SOLVING";
        case MSG_CHAIN_SOLVED: return "MSG_CHAIN_SOLVED";
        case MSG_CHAIN_END: return "MSG_CHAIN_END";
        case MSG_CHAIN_NEGATED: return "MSG_CHAIN_NEGATED";
        case MSG_CHAIN_DISABLED: return "MSG_CHAIN_DISABLED";
        case MSG_RANDOM_SELECTED: return "MSG_RANDOM_SELECTED";
        case MSG_BECOME_TARGET: return "MSG_BECOME_TARGET";
        case MSG_DRAW: return "MSG_DRAW";
        case MSG_DAMAGE: return "MSG_DAMAGE";
        case MSG_RECOVER: return "MSG_RECOVER";
        case MSG_EQUIP: return "MSG_EQUIP";
        case MSG_LPUPDATE: return "MSG_LPUPDATE";
        case MSG_CARD_TARGET: return "MSG_CARD_TARGET";
        case MSG_CANCEL_TARGET: return "MSG_CANCEL_TARGET";
        case MSG_PAY_LPCOST: return "MSG_PAY_LPCOST";
        case MSG_ADD_COUNTER: return "MSG_ADD_COUNTER";
        case MSG_REMOVE_COUNTER: return "MSG_REMOVE_COUNTER";
        case MSG_ATTACK: return "MSG_ATTACK";
        case MSG_BATTLE: return "MSG_BATTLE";
        case MSG_ATTACK_DISABLED: return "MSG_ATTACK_DISABLED";
        case MSG_DAMAGE_STEP_START: return "MSG_DAMAGE_STEP_START";
        case MSG_DAMAGE_STEP_END: return "MSG_DAMAGE_STEP_END";
        case MSG_MISSED_EFFECT: return "MSG_MISSED_EFFECT";
        case MSG_TOSS_COIN: return "MSG_TOSS_COIN";
        case MSG_TOSS_DICE: return "MSG_TOSS_DICE";
        case MSG_ROCK_PAPER_SCISSORS: return "MSG_ROCK_PAPER_SCISSORS";
        case MSG_HAND_RES: return "MSG_HAND_RES";
        case MSG_ANNOUNCE_RACE: return "MSG_ANNOUNCE_RACE";
        case MSG_ANNOUNCE_ATTRIB: return "MSG_ANNOUNCE_ATTRIB";
        case MSG_ANNOUNCE_CARD: return "MSG_ANNOUNCE_CARD";
        case MSG_ANNOUNCE_NUMBER: return "MSG_ANNOUNCE_NUMBER";
        case MSG_CARD_HINT: return "MSG_CARD_HINT";
        case MSG_TAG_SWAP: return "MSG_TAG_SWAP";
        case MSG_RELOAD_FIELD: return "MSG_RELOAD_FIELD";
        case MSG_AI_NAME: return "MSG_AI_NAME";
        case MSG_SHOW_HINT: return "MSG_SHOW_HINT";
        case MSG_HINT: return "MSG_HINT";
        case MSG_SELECT_IDLECMD: return "MSG_SELECT_IDLECMD";
        case MSG_SELECT_BATTLECMD: return "MSG_SELECT_BATTLECMD";
        case MSG_SELECT_YESNO: return "MSG_SELECT_YESNO";
        case MSG_SELECT_EFFECTYN: return "MSG_SELECT_EFFECTYN";
        case MSG_SELECT_OPTION: return "MSG_SELECT_OPTION";
        case MSG_SELECT_CARD: return "MSG_SELECT_CARD";
        case MSG_SELECT_TRIBUTE: return "MSG_SELECT_TRIBUTE";
        case MSG_SELECT_UNSELECT_CARD: return "MSG_SELECT_UNSELECT_CARD";
        case MSG_SELECT_CHAIN: return "MSG_SELECT_CHAIN";
        case MSG_SELECT_PLACE: return "MSG_SELECT_PLACE";
        case MSG_SELECT_DISFIELD: return "MSG_SELECT_DISFIELD";
        case MSG_SELECT_POSITION: return "MSG_SELECT_POSITION";
        case MSG_SELECT_SUM: return "MSG_SELECT_SUM";
        case MSG_SELECT_COUNTER: return "MSG_SELECT_COUNTER";
        case MSG_SORT_CARD: return "MSG_SORT_CARD";
#ifdef MSG_SELECT_RELEASE
        case MSG_SELECT_RELEASE: return "MSG_SELECT_RELEASE";
#endif
        case MSG_CONFIRM_DECKTOP: return "MSG_CONFIRM_DECKTOP";
        case MSG_CONFIRM_EXTRATOP: return "MSG_CONFIRM_EXTRATOP";
        case MSG_CONFIRM_CARDS: return "MSG_CONFIRM_CARDS";
        case MSG_SHUFFLE_DECK: return "MSG_SHUFFLE_DECK";
        case MSG_SHUFFLE_HAND: return "MSG_SHUFFLE_HAND";
        case MSG_SHUFFLE_EXTRA: return "MSG_SHUFFLE_EXTRA";
        case MSG_SWAP_GRAVE_DECK: return "MSG_SWAP_GRAVE_DECK";
        case MSG_REVERSE_DECK: return "MSG_REVERSE_DECK";
        case MSG_DECK_TOP: return "MSG_DECK_TOP";
        case MSG_SHUFFLE_SET_CARD: return "MSG_SHUFFLE_SET_CARD";
        default: return "MSG_UNKNOWN";
    }
}

// Deck loading
struct Deck {
    std::vector<uint32_t> main;
    std::vector<uint32_t> extra;
};

Deck load_deck(const std::string& filename) {
    Deck deck;
    std::ifstream file(filename);
    if (!file.is_open()) {
        return deck;
    }

    bool in_extra = false;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (line[0] == '!') {
            // Stop at !side or other ! sections
            break;
        }
        if (line[0] == '#') {
            if (line.find("#extra") != std::string::npos) {
                in_extra = true;
            } else {
                in_extra = false;
            }
            continue;
        }
        try {
            uint32_t id = std::stoul(line);
            if (in_extra) {
                deck.extra.push_back(id);
            } else {
                deck.main.push_back(id);
            }
        } catch (...) {}
    }
    return deck;
}

// Game state
int lp[2] = {8000, 8000};
uint32_t turn = 0;
int phase = 0;
bool mcp_mode = false;
intptr_t global_pduel = 0;

// MCP captured output: in MCP mode we redirect std::cout into this buffer
// so all existing narration code paths feed the tool result without modification.
class MCPCaptureBuf : public std::streambuf {
public:
    bool active = false;
    std::ostringstream sink;
protected:
    virtual int_type overflow(int_type c) override {
        if (active && c != EOF) sink.put((char)c);
        return c;
    }
    virtual std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (active && n > 0) sink.write(s, n);
        return n;
    }
};
MCPCaptureBuf mcp_capture;
std::streambuf* mcp_orig_cout = nullptr;

void mcp_begin_capture() {
    if (!mcp_capture.active) {
        mcp_orig_cout = std::cout.rdbuf();
        std::cout.rdbuf(&mcp_capture);
        mcp_capture.active = true;
        mcp_capture.sink.str("");
    }
}

void mcp_end_capture() {
    if (mcp_capture.active) {
        std::cout.flush();
        std::cout.rdbuf(mcp_orig_cout);
        mcp_capture.active = false;
    }
}

std::string mcp_take_capture() {
    std::string s = mcp_capture.sink.str();
    mcp_capture.sink.str("");
    return s;
}
void mcp_reset_capture() {
    mcp_capture.sink.str("");
}

// MCP pending prompt info (stored when engine stops at a prompt)
struct PendingPrompt {
    int msg_type = 0;
    int player = 0;
    std::string desc;  // human-readable description of what's being asked
    // For response construction after agent chooses
    int total_options = 0;
    // Raw buffer data for re-parsing the response type/index
    std::vector<uint8_t> msg_data;
    size_t msg_len = 0;
};
PendingPrompt mcp_pending_prompt;
bool mcp_game_over = false;
int mcp_winner = 0;
int mcp_win_reason = 0;

// Network client state. When net_client_mode is true, mcp_send_choice packs
// the raw response bytes into net_response_buf instead of calling set_responseb
// on a local engine; the ygo_client tool then sends them as CTOS_RESPONSE.
bool net_client_mode = false;
std::vector<uint8_t> net_response_buf;
int net_client_fd = -1;          // socket to the server
int net_client_seat = 0;         // our seat (0 or 1)
bool net_client_connected = false;

// Deck codes uploaded to the server (re-uploaded verbatim during match
// side-deck phases; a future ygo_side tool can replace them between games).
static std::vector<uint32_t> cli_deck_codes;

// Path of the running binary (set in main), used to locate wiki/, single/, replay/.
static std::string g_exe_dir;
static std::string g_exe_path;


// Important-card tracking: (round, code) mentioned in narration.
std::deque<std::pair<int, uint32_t>> mcp_important_cards;
int mcp_important_round = 0;

void mcp_mark_important(uint32_t code) {
    if (code == 0) return;
    mcp_important_cards.push_back({mcp_important_round, code});
    // Keep last 50 entries
    while (mcp_important_cards.size() > 50) mcp_important_cards.pop_front();
}

// Round in which each card's full detail was last shown (0 round = never).
std::unordered_map<uint32_t, int> mcp_last_detail_round;

// Record that a card's stats/detail were shown this round.
void mcp_mark_detailed(uint32_t code) {
    if (code == 0) return;
    mcp_last_detail_round[code] = mcp_important_round;
}

// All card codes currently visible on the field (both players, all zones).
// Used to decide which cards warrant a detail block even without narration.
std::unordered_set<uint32_t> mcp_collect_visible_codes() {
    std::unordered_set<uint32_t> codes;
    uint8_t buffer[8192];
    static const uint32_t zones[] = {LOCATION_HAND, LOCATION_MZONE, LOCATION_SZONE,
                                     LOCATION_GRAVE, LOCATION_REMOVED};
    for (int player = 0; player < 2; ++player) {
        for (uint32_t loc : zones) {
            int len = query_field_card(global_pduel, player, loc, QUERY_CODE, buffer, 0);
            if (len <= 0) continue;
            uint8_t* p = buffer;
            while (p - buffer < len) {
                uint8_t* card_start = p;
                int32_t card_len = BufferIO::Read<int32_t>(p);
                if (card_len > 4) {
                    uint8_t* card_p = p;
                    int32_t flag = BufferIO::Read<int32_t>(card_p);
                    if (flag & QUERY_CODE) {
                        uint32_t code = BufferIO::Read<uint32_t>(card_p);
                        if (code) codes.insert(code);
                    }
                }
                p = card_start + card_len;
            }
        }
    }
    return codes;
}

// Cards that need a full detail block: currently visible OR mentioned in the
// last 3 rounds, whose detail hasn't been shown within the last 3 rounds.
// Deduped: each card appears at most once.
std::vector<uint32_t> mcp_get_cards_needing_detail() {
    std::vector<uint32_t> result;
    std::unordered_set<uint32_t> candidates;
    for (uint32_t code : mcp_collect_visible_codes()) candidates.insert(code);
    for (auto it = mcp_important_cards.rbegin(); it != mcp_important_cards.rend(); ++it) {
        if (mcp_important_round - it->first < 3) candidates.insert(it->second);
    }
    for (uint32_t code : candidates) {
        if (code == 0) continue;
        auto it = mcp_last_detail_round.find(code);
        int last = (it == mcp_last_detail_round.end()) ? -1 : it->second;
        // Never-seen cards always need detail; otherwise refresh after 3 rounds.
        if (last == -1 || mcp_important_round - last >= 3) result.push_back(code);
    }
    return result;
}

void print_core_error(const std::string& message) {
    if (message.empty()) {
        return;
    }
    const char* red = "\033[31m";
    const char* reset = "\033[0m";
    std::cerr << red << "[CORE] " << message << reset << "\n";
}

// ======= MCP helpers =======

// JSON handled by nlohmann::json (see <nlohmann/json.hpp>).
// Tool params arrive as parsed json values; responses are built as json objects
// and serialized with dump(), which handles escaping automatically.

// Card type string (English, readable)
std::string card_type_en(uint32_t type) {
    std::vector<std::string> parts;
    struct { uint32_t flag; const char* name; } list[] = {
        {TYPE_MONSTER, "Monster"}, {TYPE_SPELL, "Spell"}, {TYPE_TRAP, "Trap"},
        {TYPE_NORMAL, "Normal"}, {TYPE_EFFECT, "Effect"}, {TYPE_FUSION, "Fusion"},
        {TYPE_RITUAL, "Ritual"}, {TYPE_SPIRIT, "Spirit"}, {TYPE_UNION, "Union"},
        {TYPE_DUAL, "Dual"}, {TYPE_TUNER, "Tuner"}, {TYPE_SYNCHRO, "Synchro"},
        {TYPE_TOKEN, "Token"}, {TYPE_QUICKPLAY, "Quick-Play"}, {TYPE_CONTINUOUS, "Continuous"},
        {TYPE_EQUIP, "Equip"}, {TYPE_FIELD, "Field"}, {TYPE_COUNTER, "Counter"},
        {TYPE_FLIP, "Flip"}, {TYPE_TOON, "Toon"}, {TYPE_XYZ, "Xyz"},
        {TYPE_PENDULUM, "Pendulum"}, {TYPE_SPSUMMON, "Special Summon"}, {TYPE_LINK, "Link"},
    };
    for (auto& e : list) if (type & e.flag) parts.push_back(e.name);
    if (parts.empty()) return "Monster";
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out += "|"; out += parts[i]; }
    return out;
}

// Race / attribute English names
std::string race_name(uint32_t race) {
    static const std::pair<uint32_t, const char*> list[] = {
        {RACE_WARRIOR,"Warrior"},{RACE_SPELLCASTER,"Spellcaster"},{RACE_FAIRY,"Fairy"},
        {RACE_FIEND,"Fiend"},{RACE_ZOMBIE,"Zombie"},{RACE_MACHINE,"Machine"},
        {RACE_AQUA,"Aqua"},{RACE_PYRO,"Pyro"},{RACE_ROCK,"Rock"},{RACE_WINDBEAST,"Winged Beast"},
        {RACE_PLANT,"Plant"},{RACE_INSECT,"Insect"},{RACE_THUNDER,"Thunder"},
        {RACE_DRAGON,"Dragon"},{RACE_BEAST,"Beast"},{RACE_BEASTWARRIOR,"Beast-Warrior"},
        {RACE_DINOSAUR,"Dinosaur"},{RACE_FISH,"Fish"},{RACE_SEASERPENT,"Sea Serpent"},
        {RACE_REPTILE,"Reptile"},{RACE_PSYCHO,"Psychic"},{RACE_DEVINE,"Divine-Beast"},
        {RACE_CREATORGOD,"Creator God"},{RACE_WYRM,"Wyrm"},{RACE_CYBERSE,"Cyberse"},
        {RACE_ILLUSION,"Illusion"},
    };
    for (auto& e : list) if (race == e.first) return e.second;
    return "?";
}

std::string attribute_name(uint32_t attr) {
    switch (attr) {
        case ATTRIBUTE_EARTH: return "Earth";
        case ATTRIBUTE_WATER: return "Water";
        case ATTRIBUTE_FIRE: return "Fire";
        case ATTRIBUTE_WIND: return "Wind";
        case ATTRIBUTE_LIGHT: return "Light";
        case ATTRIBUTE_DARK: return "Dark";
        case ATTRIBUTE_DEVINE: return "Divine";
        default: return "?";
    }
}

// Compact card detail line (single line, token-efficient)
std::string card_detail_line(uint32_t code) {
    auto dit = card_datas.find(code);
    if (dit == card_datas.end()) {
        return get_card_name(code) + " (no data)";
    }
    const auto& d = dit->second;
    std::string out = get_card_name(code);
    std::string type_str = card_type_en(d.type);
    if (d.type & TYPE_MONSTER) {
        uint32_t lv = d.level & 0xff;
        out += " " + race_name(d.race) + "/" + attribute_name(d.attribute);
        if (d.type & TYPE_LINK) {
            out += " " + std::to_string(d.attack) + " [Link " + std::to_string(lv) + "]";
        } else {
            // Xyz monsters store their rank in the level field; mark it as R.
            out += " " + std::string((d.type & TYPE_XYZ) ? "R" : "★") + std::to_string(lv);
            out += " " + std::to_string(d.attack) + "/" + std::to_string(d.defense);
        }
    }
    out += " [" + type_str + "]";
    return out;
}

// Full card detail (name line + desc). Cards with no DB entry (e.g. script-
// generated Xyz overlay units) are skipped entirely — nothing useful to show.
std::string card_detail_full(uint32_t code) {
    auto dit = card_datas.find(code);
    if (dit == card_datas.end()) return "";
    std::string out = "[Card] " + card_detail_line(code) + "\n";
    const auto& d = dit->second;
    uint32_t lscale = d.lscale, rscale = d.rscale, link = d.link_marker;
    if (d.type & TYPE_PENDULUM && (lscale || rscale)) {
        out += "  Pendulum: " + std::to_string(lscale) + "/" + std::to_string(rscale) + "\n";
    }
    if (d.type & TYPE_LINK) {
        int arrows = 0;
        std::string arms;
        for (int i = 0; i < 8; i++) if (link & (1u << i)) arrows++;
        out += "  Link: " + std::to_string(d.attack) + " (" + std::to_string(arrows) + " arrows)\n";
    }
    std::string desc = normalize_effect_text(card_descs.count(code) ? card_descs[code] : "");
    if (!desc.empty() && desc != "-") {
        out += "  " + desc + "\n";
    }
    return out;
}

// forward declaration
std::string mcp_location_cards_text(int player, uint32_t location);

// MCP game state serialization (text format).
// Name-only by default; MZone also shows current position/level/atk/def
// (these change with effects); SZone shows face-up/face-down.
std::string mcp_game_state_text() {
    std::ostringstream out;
    out << "=== Turn " << turn << " Phase " << phase_name(phase) << " ===\n";
    for (int player = 0; player < 2; ++player) {
        out << "P" << player << " LP:" << lp[player]
            << " Deck:" << query_field_count(global_pduel, player, LOCATION_DECK)
            << " Extra:" << query_field_count(global_pduel, player, LOCATION_EXTRA) << "\n";
        out << "  Hand: " << (mcp_location_cards_text(player, LOCATION_HAND).empty() ? "-" : mcp_location_cards_text(player, LOCATION_HAND)) << "\n";
        out << "  MZone: " << (mcp_location_cards_text(player, LOCATION_MZONE).empty() ? "-" : mcp_location_cards_text(player, LOCATION_MZONE)) << "\n";
        out << "  SZone: " << (mcp_location_cards_text(player, LOCATION_SZONE).empty() ? "-" : mcp_location_cards_text(player, LOCATION_SZONE)) << "\n";
        out << "  Grave: " << (mcp_location_cards_text(player, LOCATION_GRAVE).empty() ? "-" : mcp_location_cards_text(player, LOCATION_GRAVE)) << "\n";
        out << "  Removed: " << (mcp_location_cards_text(player, LOCATION_REMOVED).empty() ? "-" : mcp_location_cards_text(player, LOCATION_REMOVED)) << "\n";
    }
    return out.str();
}

// One field-location listing: name only, except MZone (pos/level/atk/def) and
// SZone (UP/FD). The zone sequence comes from the query's info_location.
std::string mcp_location_cards_text(int player, uint32_t location) {
    uint8_t buffer[8192];
    uint32_t query_flags = QUERY_CODE | QUERY_POSITION;
    bool is_mzone = (location == LOCATION_MZONE);
    bool is_szone = (location == LOCATION_SZONE);
    if (is_mzone) query_flags |= QUERY_ATTACK | QUERY_DEFENSE | QUERY_LEVEL | QUERY_RANK;
    int len = query_field_card(global_pduel, player, location, query_flags, buffer, 0);
    std::ostringstream out;
    if (len > 0) {
        uint8_t* p = buffer;
        bool first = true;
        while (p - buffer < len) {
            uint8_t* card_start = p;
            int32_t card_len = BufferIO::Read<int32_t>(p);
            if (card_len > 4) {
                uint8_t* card_p = p;
                int32_t flag = BufferIO::Read<int32_t>(card_p);
                uint32_t code = 0;
                uint32_t info_loc = 0;  // c + l<<8 + s<<16 + pos<<24
                uint32_t level = 0, rank = 0;
                int32_t atk = 0, def = 0;
                if (flag & QUERY_CODE) code = BufferIO::Read<uint32_t>(card_p);
                if (flag & QUERY_POSITION) info_loc = BufferIO::Read<uint32_t>(card_p);
                if (flag & QUERY_ALIAS) BufferIO::Read<uint32_t>(card_p);
                if (flag & QUERY_TYPE) BufferIO::Read<uint32_t>(card_p);
                if (flag & QUERY_LEVEL) level = BufferIO::Read<uint32_t>(card_p);
                if (flag & QUERY_RANK) rank = BufferIO::Read<uint32_t>(card_p);
                if (flag & QUERY_ATTRIBUTE) BufferIO::Read<uint32_t>(card_p);
                if (flag & QUERY_RACE) BufferIO::Read<uint32_t>(card_p);
                if (flag & QUERY_ATTACK) atk = BufferIO::Read<int32_t>(card_p);
                if (flag & QUERY_DEFENSE) def = BufferIO::Read<int32_t>(card_p);
                if (code != 0) {
                    if (!first) out << ", ";
                    first = false;
                    uint8_t pos = (info_loc >> 24) & 0xff;
                    if (is_mzone || is_szone) out << "[" << ((info_loc >> 16) & 0xff) << "] ";
                    out << get_card_name(code);
                    if (is_mzone) {
                        out << " " << pos_name(pos);
                        out << mcp_lv_label(code, level, rank);
                        out << " " << atk << "/" << def;
                    } else if (is_szone) {
                        out << " " << st_pos_label(pos);
                    }
                }
            }
            p = card_start + card_len;
        }
    }
    return out.str();
}

// Output captured narration + field state + important card detail + choice list
std::string mcp_format_output(const std::string& narration, const std::string& choices) {
    std::ostringstream out;
    // Narration
    if (!narration.empty()) out << narration;
    // Field state
    out << mcp_game_state_text();
    // Important card details (recency window, deduped per card)
    for (uint32_t code : mcp_get_cards_needing_detail()) {
        out << card_detail_full(code);
        mcp_mark_detailed(code);
    }
    // Choice list
    if (!choices.empty()) out << choices;
    return out.str();
}

// ======= MCP engine =======
// Self-contained engine loop that runs until a prompt or game end.
// Stores prompt info in mcp_pending_prompt. Narration captured via std::cout redirect.
// Returns: 0 = prompt pending, 1 = game over, -1 = game not started

struct MCPLastResponse {
    bool valid = false;
    bool is_buffer = false;
    int32_t int_val = 0;
    std::vector<uint8_t> buf;
};
MCPLastResponse mcp_last_response;

// MCP engine buffer (persistent across calls)
std::vector<uint8_t> mcp_engine_buffer(SIZE_MESSAGE_BUFFER);

// Reset MCP engine state for a new game
void mcp_reset_game_state() {
    mcp_last_response = MCPLastResponse();
    mcp_pending_prompt = PendingPrompt();
    mcp_game_over = false;
    mcp_winner = 0;
    mcp_win_reason = 0;
    mcp_important_cards.clear();
    mcp_important_round = 0;
    turn = 0;
    phase = 0;
    lp[0] = lp[1] = 8000;
}

// Store raw message data for prompt
void mcp_store_prompt_raw(int msg_type, int player, uint8_t* data, size_t len) {
    mcp_pending_prompt = PendingPrompt();
    mcp_pending_prompt.msg_type = msg_type;
    mcp_pending_prompt.player = player;
    mcp_pending_prompt.msg_data.assign(data, data + len);
    mcp_pending_prompt.msg_len = len;
}

// Forward declaration (defined below).
static void mcp_send_choice(int choice_idx, const std::vector<int>& choice_indices);

// Safety cap on automatic responses within one engine run: if the core keeps
// producing forced prompts that never advance the game, stop auto-answering and
// surface the current prompt to the agent instead of spinning forever.
static const int MCP_MAX_AUTO_RESPONSES = 10000;

// Does the pending prompt actually require a decision? The core sometimes emits
// prompts with no real options: an idle prompt whose only legal move is to end
// the turn, a chain prompt with zero chains (only pass), a sort with <=1 card,
// etc. For those the agent would just send -1 (pass) or the single option, so
// answer them automatically and keep running instead of waking the agent.
// If forced, *auto_idx is set to the choice index that reproduces the forced
// response (usually -1 = pass) and true is returned; otherwise false.
static bool mcp_prompt_is_forced(int& auto_idx) {
    auto_idx = -1;
    const auto& pp = mcp_pending_prompt;
    if (pp.msg_data.empty()) return false;
    uint8_t* p = const_cast<uint8_t*>(pp.msg_data.data());
    uint8_t* end = p + pp.msg_data.size();

    auto rd8  = [&]() -> uint8_t  { if (p < end) return *p++; return 0; };
    auto rd16 = [&]() -> uint16_t { uint16_t v = 0; if (p + 2 <= end) std::memcpy(&v, p, 2); p += 2; return v; };
    auto rd32 = [&]() -> uint32_t { uint32_t v = 0; if (p + 4 <= end) std::memcpy(&v, p, 4); p += 4; return v; };
    auto skip7  = [&](int n) { for (int i = 0; i < n; i++) { rd32(); rd8(); rd8(); rd8(); } };
    auto skip8  = [&](int n) { for (int i = 0; i < n; i++) { rd32(); rd8(); rd8(); rd8(); rd8(); } };
    auto skip11 = [&](int n) { for (int i = 0; i < n; i++) { rd32(); rd8(); rd8(); rd8(); rd32(); } };

    switch (pp.msg_type) {
        case MSG_SELECT_IDLECMD: {
            rd8(); // player
            int total = 0;
            uint8_t c;
            c = rd8(); total += c; skip7(c);   // summon
            c = rd8(); total += c; skip7(c);   // special summon
            c = rd8(); total += c; skip7(c);   // reposition
            c = rd8(); total += c; skip7(c);   // set monster
            c = rd8(); total += c; skip7(c);   // set spell/trap
            c = rd8(); total += c; skip11(c);  // activate
            uint8_t bp = rd8();
            uint8_t ep = rd8();
            uint8_t shuffle = rd8();
            // No playable action, exactly one phase transition, and no shuffle:
            // nothing to decide, pass (mcp_send_choice maps -1 to End/Battle
            // Phase). If both bp and ep (or shuffle) are present it's a real
            // choice; if neither is present the pass fallback would be invalid.
            if (total == 0 && bp + ep == 1 && shuffle == 0) { auto_idx = -1; return true; }
            return false;
        }
        case MSG_SELECT_BATTLECMD: {
            rd8(); // player
            uint8_t ac = rd8(); skip11(ac);    // activate
            uint8_t at = rd8(); skip8(at);     // attack
            uint8_t m2 = rd8();
            uint8_t ep = rd8();
            // No attack/activation and exactly one phase transition: forced pass
            // (maps to Main Phase 2 or End Phase). Both present = real choice;
            // neither present = pass would be invalid, keep it a real prompt.
            if (ac + at == 0 && m2 + ep == 1) { auto_idx = -1; return true; }
            return false;
        }
        case MSG_SELECT_CHAIN: {
            rd8(); // player
            uint8_t cnt = rd8(); // chain count; zero chains => only pass
            if (cnt == 0) { auto_idx = -1; return true; }
            return false;
        }
        // The degenerate cases below never actually reach the client: the core
        // auto-resolves empty card/option/sum/counter prompts (select_card /
        // select_option / select_unselect_card / select_with_sum_limit /
        // select_counter / select_position return TRUE when there is nothing to
        // pick). They are kept as defensive no-ops in case a future core change
        // emits them.
        case MSG_SELECT_CARD:
        case MSG_SELECT_TRIBUTE: {
            rd8(); // player
            uint8_t cancelable = rd8();
            uint8_t min = rd8();
            rd8(); // max
            uint8_t cnt = rd8();
            if (cnt == 0 && min == 0 && cancelable != 0) { auto_idx = -1; return true; }
            return false;
        }
        case MSG_SELECT_UNSELECT_CARD: {
            rd8(); // player
            uint8_t finishable = rd8();
            rd8(); // cancelable
            uint8_t min = rd8();
            rd8(); // max
            uint8_t cnt = rd8();
            if (cnt == 0 && min == 0 && finishable != 0) { auto_idx = -1; return true; }
            return false;
        }
        case MSG_SELECT_OPTION: {
            rd8(); // player
            uint8_t cnt = rd8();
            if (cnt == 0) { auto_idx = -1; return true; }
            return false;
        }
        case MSG_SELECT_POSITION: {
            rd8(); // player
            rd32(); // code
            uint8_t pos = rd8();
            int n = (pos & 0x1) + ((pos >> 1) & 0x1) + ((pos >> 2) & 0x1) + ((pos >> 3) & 0x1);
            if (n == 1) { auto_idx = 0; return true; }
            return false;
        }
        case MSG_SELECT_SUM: {
            rd8(); // mode
            rd8(); // player
            rd32(); // acc
            rd8(); rd8(); // min, max
            uint8_t mcnt = rd8(); skip11(mcnt);
            uint8_t scnt = rd8();
            if (mcnt == 0 && scnt == 0) { auto_idx = -1; return true; }
            return false;
        }
        case MSG_SELECT_COUNTER: {
            rd8(); // player
            rd16(); // countertype
            rd16(); // count
            uint8_t cnt = rd8();
            if (cnt == 0) { auto_idx = 0; return true; }
            return false;
        }
        // MSG_SORT_CARD is intentionally NOT auto-answered: the core emits it
        // even for a single card, and the response is a permutation (bvalue
        // bytes) that mcp_send_choice cannot build correctly, so it stays a
        // real (agent) choice.
        default:
            return false;
    }
}

// Run the engine until a prompt is encountered or the game ends.
// Narration goes to std::cout (captured by mcp_capture).
// Prompts with no real options are answered automatically and never pause.
// On prompt: stores prompt info, returns 0.
// On game end: returns 1 (mcp_winner/mcp_win_reason set).
// Call mcp_send_choice() before calling this again to respond to the prompt.
int mcp_run_until_pause() {
    if (global_pduel == 0) return -1;
    intptr_t pduel = global_pduel;
    int auto_responds = 0;

    while (true) {
        uint32_t result = process(pduel);
        if (result & PROCESSOR_END) {
            std::cout << "Game ended naturally.\n";
            mcp_game_over = true;
            return 1;
        }

        uint32_t len = result & PROCESSOR_BUFFER_LEN;
        if (len > 0) {
            get_message(pduel, mcp_engine_buffer.data());
            if (std::getenv("YGOCLI_MCP_DEBUG")) {
                fprintf(stderr, "[dbg] buffer len=%u:", len);
                for (uint32_t i = 0; i < len && i < 48; ++i)
                    fprintf(stderr, " %02x", mcp_engine_buffer[i]);
                fprintf(stderr, "\n");
            }
            uint8_t* pbuf = mcp_engine_buffer.data();
            uint8_t* msg_buffer = mcp_engine_buffer.data();

            while (pbuf - msg_buffer < (int)len) {
                uint8_t msg_type = BufferIO::Read<uint8_t>(pbuf);
                if (std::getenv("YGOCLI_MCP_DEBUG")) {
                    std::fprintf(stderr, "[MCP] msg_type=%d\n", (int)msg_type);
                }

                // Determine if this is a prompt-type message that requires user input
                bool is_prompt = false;
                switch (msg_type) {
                    case MSG_SELECT_IDLECMD:
                    case MSG_SELECT_BATTLECMD:
                    case MSG_SELECT_YESNO:
                    case MSG_SELECT_EFFECTYN:
                    case MSG_SELECT_OPTION:
                    case MSG_SELECT_CARD:
                    case MSG_SELECT_TRIBUTE:
                    case MSG_SELECT_UNSELECT_CARD:
                    case MSG_SELECT_CHAIN:
                    case MSG_SELECT_PLACE:
                    case MSG_SELECT_DISFIELD:
                    case MSG_SELECT_POSITION:
                    case MSG_SELECT_SUM:
                    case MSG_SELECT_COUNTER:
                    case MSG_SORT_CARD:
                    case MSG_ROCK_PAPER_SCISSORS:
                    case MSG_ANNOUNCE_RACE:
                    case MSG_ANNOUNCE_ATTRIB:
                    case MSG_ANNOUNCE_CARD:
                    case MSG_ANNOUNCE_NUMBER:
                        is_prompt = true;
                        break;
                }

                if (is_prompt) {
                    // Store prompt data (everything after the msg_type byte)
                    uint8_t* prompt_data = pbuf;
                    size_t remaining = len - (pbuf - msg_buffer);
                    mcp_store_prompt_raw(msg_type, 0, prompt_data, remaining);

                    // A prompt with no real decision (only pass / a single
                    // forced option) is answered automatically so the game
                    // keeps running and the agent is not woken until a genuine
                    // choice or the duel ends.
                    int auto_idx = -1;
                    bool forced = mcp_prompt_is_forced(auto_idx);
                    if (std::getenv("YGOCLI_MCP_DEBUG"))
                        fprintf(stderr, "[dbg] prompt type=%d forced=%d auto_idx=%d len=%zu\n",
                                msg_type, (int)forced, auto_idx, remaining);
                    if (forced && auto_responds < MCP_MAX_AUTO_RESPONSES) {
                        ++auto_responds;
                        std::cout << "[auto] " << msg_type_name(msg_type)
                                  << ": no real choice, responding automatically\n";
                        mcp_send_choice(auto_idx, {});
                        // The prompt is the last message of this batch; skip the
                        // rest and let the engine process our auto response.
                        pbuf = msg_buffer + len;
                        continue;
                    }
                    if (std::getenv("YGOCLI_MCP_DEBUG"))
                        fprintf(stderr, "[dbg] PAUSE at prompt type=%d\n", msg_type);
                    return 0;
                }

                // Handle informational messages for narration
                switch (msg_type) {
                    case MSG_RETRY:
                        // The engine rejected the previous response and will keep
                        // re-validating the stored one until we send a new value
                        // (e.g. passing a chain that contains a forced chain).
                        // Re-surface the stored prompt so the agent can choose
                        // again; the next ygo_choose overwrites the response.
                        if (std::getenv("YGOCLI_MCP_DEBUG"))
                            fprintf(stderr, "[dbg] RETRY stored type=%d len=%zu\n",
                                    mcp_pending_prompt.msg_type, mcp_pending_prompt.msg_len);
                        if (!mcp_pending_prompt.msg_data.empty()) {
                            return 0;
                        }
                        break;
                    case MSG_WIN: {
                        uint8_t w = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t r = BufferIO::Read<uint8_t>(pbuf);
                        mcp_winner = w;
                        mcp_win_reason = r;
                        mcp_game_over = true;
                        std::cout << "Player " << (int)w << " wins! (reason " << (int)r << ")\n";
                        return 1;
                    }
                    case MSG_NEW_TURN: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        turn++;
                        mcp_important_round++;
                                            std::cout << "=== Turn " << turn << " (Player " << (int)pl << ") ===\n";
                        break;
                    }
                    case MSG_NEW_PHASE: {
                        phase = BufferIO::Read<uint16_t>(pbuf);
                        std::cout << "Phase: " << phase_name(phase) << "\n";
                        break;
                    }
                    case MSG_LPUPDATE: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        lp[pl] = BufferIO::Read<int32_t>(pbuf);
                        std::cout << "P" << (int)pl << " LP: " << lp[pl] << "\n";
                        break;
                    }
                    case MSG_DAMAGE: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        int32_t dmg = BufferIO::Read<int32_t>(pbuf);
                        lp[pl] -= dmg;
                        if (lp[pl] < 0) lp[pl] = 0;
                        std::cout << "P" << (int)pl << " takes " << dmg << " damage (LP: " << lp[pl] << ")\n";
                        break;
                    }
                    case MSG_RECOVER: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        int32_t rec = BufferIO::Read<int32_t>(pbuf);
                        lp[pl] += rec;
                        std::cout << "P" << (int)pl << " recovers " << rec << " LP (LP: " << lp[pl] << ")\n";
                        break;
                    }
                    case MSG_PAY_LPCOST: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        int32_t cost = BufferIO::Read<int32_t>(pbuf);
                        lp[pl] -= cost;
                        if (lp[pl] < 0) lp[pl] = 0;
                        std::cout << "P" << (int)pl << " pays " << cost << " LP (LP: " << lp[pl] << ")\n";
                        break;
                    }
                    case MSG_DRAW: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "P" << (int)pl << " draws " << (int)cnt << " cards: ";
                        for (int i = 0; i < cnt; i++) {
                            uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                            if (i > 0) std::cout << ", ";
                            std::cout << get_card_name(code);
                            mcp_mark_important(code);
                        }
                        std::cout << "\n";
                        break;
                    }
                    case MSG_MOVE: {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t prev_ctrl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t prev_loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t prev_seq = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t prev_pos = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t new_ctrl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t new_loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t new_seq = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t new_pos = BufferIO::Read<uint8_t>(pbuf);
                        int32_t reason = BufferIO::Read<int32_t>(pbuf);
                        (void)prev_ctrl;(void)prev_pos;(void)new_pos;(void)reason;
                        std::cout << get_card_name(code) << " moved: "
                                  << location_name(prev_loc) << "[" << (int)prev_seq << "]"
                                  << " -> " << location_name(new_loc) << "[" << (int)new_seq << "]"
                                  << " (P" << (int)new_ctrl << ")\n";
                        // Mark as important if moving to a visible field
                        if (new_loc == LOCATION_MZONE || new_loc == LOCATION_SZONE || new_loc == LOCATION_HAND
                            || new_loc == LOCATION_GRAVE || new_loc == LOCATION_REMOVED)
                            mcp_mark_important(code);
                        break;
                    }
                    case MSG_SUMMONING: {
                        // payload: code(4) + info_location(4) (ctrl+loc<<8+seq<<16+pos<<24)
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint32_t il = BufferIO::Read<uint32_t>(pbuf);
                        (void)il;
                        std::cout << "Summoning " << get_card_name(code) << "\n";
                        break;
                    }
                    case MSG_SPSUMMONING: {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint32_t il = BufferIO::Read<uint32_t>(pbuf);
                        (void)il;
                        std::cout << "Special Summoning " << get_card_name(code) << "\n";
                        break;
                    }
                    case MSG_FLIPSUMMONING: {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint32_t il = BufferIO::Read<uint32_t>(pbuf);
                        (void)il;
                        std::cout << "Flip Summoning " << get_card_name(code) << "\n";
                        break;
                    }
                    case MSG_SUMMONED:
                    case MSG_SPSUMMONED:
                    case MSG_FLIPSUMMONED:
                        std::cout << "Summon successful.\n";
                        break;
                    case MSG_CHAINING: {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t sub_seq = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t trig_pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t trig_loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t trig_seq = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t desc = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t chain_cnt = BufferIO::Read<uint8_t>(pbuf);
                        (void)pl;(void)loc;(void)seq;(void)sub_seq;(void)trig_pl;(void)trig_loc;(void)trig_seq;(void)desc;
                        std::cout << "Chain " << (int)chain_cnt << ": " << get_card_name(code) << "\n";
                        mcp_mark_important(code);
                        break;
                    }
                    case MSG_CHAINED: {
                        // payload is just the chain_count byte (no card code)
                        uint8_t chain_cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "Chain " << (int)chain_cnt << " added.\n";
                        break;
                    }
                    case MSG_CHAIN_SOLVING: {
                        uint8_t chain_cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "Solving chain " << (int)chain_cnt << "\n";
                        break;
                    }
                    case MSG_CHAIN_SOLVED: {
                        uint8_t chain_cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "Chain " << (int)chain_cnt << " solved.\n";
                        break;
                    }
                    case MSG_CHAIN_END:
                        std::cout << "Chain ended.\n";
                        break;
                    case MSG_CHAIN_NEGATED: {
                        uint8_t chain_cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "Chain " << (int)chain_cnt << " negated.\n";
                        break;
                    }
                    case MSG_CHAIN_DISABLED: {
                        uint8_t chain_cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "Chain " << (int)chain_cnt << " disabled.\n";
                        break;
                    }
                    case MSG_ATTACK: {
                        uint8_t atk_pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t atk_loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t atk_seq = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t atk_pos = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t def_pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t def_loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t def_seq = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t def_pos = BufferIO::Read<uint8_t>(pbuf);
                        (void)atk_pos;(void)def_pos;
                        std::cout << "Attack: P" << (int)atk_pl << " " << location_name(atk_loc) << "[" << (int)atk_seq << "]"
                                  << " -> P" << (int)def_pl;
                        if (def_loc == 0) std::cout << " (direct)";
                        else std::cout << " " << location_name(def_loc) << "[" << (int)def_seq << "]";
                        std::cout << "\n";
                        break;
                    }
                    case MSG_BATTLE: {
                        uint8_t ap = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t al = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t as = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t apos = BufferIO::Read<uint8_t>(pbuf);
                        int32_t aatk = BufferIO::Read<int32_t>(pbuf);
                        int32_t adef = BufferIO::Read<int32_t>(pbuf);
                        uint8_t ad = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t dp = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t dl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t ds = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t dpos = BufferIO::Read<uint8_t>(pbuf);
                        int32_t datk = BufferIO::Read<int32_t>(pbuf);
                        int32_t ddef = BufferIO::Read<int32_t>(pbuf);
                        uint8_t dd = BufferIO::Read<uint8_t>(pbuf);
                        (void)apos;(void)dpos;
                        std::cout << "Battle: P" << (int)ap << " " << location_name(al) << "[" << (int)as << "]"
                                  << " (ATK:" << aatk << "/DEF:" << adef << (ad?" destroyed":"")
                                  << ") vs P" << (int)dp << " ";
                        if (dl == 0) std::cout << "(direct)";
                        else std::cout << location_name(dl) << "[" << (int)ds << "]";
                        std::cout << " (ATK:" << datk << "/DEF:" << ddef << (dd?" destroyed":"") << ")\n";
                        break;
                    }
                    case MSG_ATTACK_DISABLED:
                        std::cout << "Attack disabled.\n";
                        break;
                    case MSG_DAMAGE_STEP_START:
                        std::cout << "Damage step start.\n";
                        break;
                    case MSG_DAMAGE_STEP_END:
                        std::cout << "Damage step end.\n";
                        break;
                    case MSG_SHUFFLE_DECK: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "P" << (int)pl << " deck shuffled.\n";
                        break;
                    }
                    case MSG_SHUFFLE_HAND: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        // Payload: cnt card codes (4 bytes each). Skip to stay in sync.
                        for (int i = 0; i < cnt; i++) BufferIO::Read<uint32_t>(pbuf);
                        std::cout << "P" << (int)pl << " hand shuffled.\n";
                        break;
                    }
                    case MSG_SHUFFLE_EXTRA: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        // Payload: cnt card codes (4 bytes each). Skip to stay in sync.
                        for (int i = 0; i < cnt; i++) BufferIO::Read<uint32_t>(pbuf);
                        std::cout << "P" << (int)pl << " extra deck shuffled.\n";
                        break;
                    }
                    case MSG_POS_CHANGE: {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t prev = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t next = BufferIO::Read<uint8_t>(pbuf);
                        (void)pl;(void)prev;
                        std::cout << get_card_name(code) << " " << location_name(loc)
                                  << "[" << (int)seq << "] pos -> " << pos_name(next) << "\n";
                        break;
                    }
                    case MSG_SET: {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t pos = BufferIO::Read<uint8_t>(pbuf);
                        (void)pl;(void)pos;
                        std::cout << get_card_name(code) << " set " << location_name(loc)
                                  << "[" << (int)seq << "]\n";
                        break;
                    }
                    case MSG_SWAP: {
                        uint32_t c1 = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t p1 = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t l1 = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t s1 = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t pos1 = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t c2 = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t p2 = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t l2 = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t s2 = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t pos2 = BufferIO::Read<uint8_t>(pbuf);
                        (void)p1;(void)l1;(void)s1;(void)pos1;(void)p2;(void)l2;(void)s2;(void)pos2;
                        std::cout << "Swapped " << get_card_name(c1) << " and " << get_card_name(c2) << "\n";
                        break;
                    }
                    case MSG_FIELD_DISABLED: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t disabled = BufferIO::Read<uint32_t>(pbuf);
                        std::cout << "P" << (int)pl << " field disabled: 0x" << std::hex << disabled << std::dec << "\n";
                        break;
                    }
                    case MSG_BECOME_TARGET: {
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "Targeted: ";
                        for (int i = 0; i < cnt; i++) {
                            uint32_t code = BufferIO::Read<int32_t>(pbuf);
                            if (i > 0) std::cout << ", ";
                            std::cout << get_card_name(code);
                        }
                        std::cout << "\n";
                        break;
                    }
                    case MSG_CARD_TARGET: {
                        // payload: source info_location(4) + target info_location(4)
                        uint32_t src = BufferIO::Read<uint32_t>(pbuf);
                        uint32_t tgt = BufferIO::Read<uint32_t>(pbuf);
                        (void)src;
                        uint8_t t_ctrl = tgt & 0xff;
                        uint8_t t_loc = (tgt >> 8) & 0xff;
                        uint8_t t_seq = (tgt >> 16) & 0xff;
                        std::cout << "Targets @" << location_name(t_loc) << "[" << (int)t_seq << "]"
                                  << " (P" << (int)t_ctrl << ")\n";
                        break;
                    }
                    case MSG_CANCEL_TARGET: {
                        uint32_t src = BufferIO::Read<uint32_t>(pbuf);
                        uint32_t tgt = BufferIO::Read<uint32_t>(pbuf);
                        (void)src;
                        uint8_t t_loc = (tgt >> 8) & 0xff;
                        uint8_t t_seq = (tgt >> 16) & 0xff;
                        std::cout << "Cancel target @" << location_name(t_loc) << "[" << (int)t_seq << "]\n";
                        break;
                    }
                    case MSG_EQUIP: {
                        // payload: equip info_location(4) + target info_location(4)
                        uint32_t eq = BufferIO::Read<uint32_t>(pbuf);
                        uint32_t tgt = BufferIO::Read<uint32_t>(pbuf);
                        std::cout << "Equip: " << location_name((eq >> 8) & 0xff) << "[" << ((eq >> 16) & 0xff) << "]"
                                  << " -> " << location_name((tgt >> 8) & 0xff) << "[" << ((tgt >> 16) & 0xff) << "]\n";
                        break;
                    }
                    case MSG_UNEQUIP: {
                        // payload: info_location(4)
                        pbuf += 4;
                        break;
                    }
                    case MSG_ADD_COUNTER: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint16_t cnt_type = BufferIO::Read<uint16_t>(pbuf);
                        uint16_t count = BufferIO::Read<uint16_t>(pbuf);
                        (void)pl;(void)loc;(void)seq;
                        std::cout << "Added " << count << " counters (type " << cnt_type << ")\n";
                        break;
                    }
                    case MSG_REMOVE_COUNTER: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint16_t cnt_type = BufferIO::Read<uint16_t>(pbuf);
                        uint16_t count = BufferIO::Read<uint16_t>(pbuf);
                        (void)pl;(void)loc;(void)seq;
                        std::cout << "Removed " << count << " counters (type " << cnt_type << ")\n";
                        break;
                    }
                    case MSG_TOSS_COIN: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "P" << (int)pl << " coin toss: ";
                        for (int i = 0; i < cnt; i++) {
                            uint8_t r = BufferIO::Read<uint8_t>(pbuf);
                            if (i > 0) std::cout << ", ";
                            std::cout << (r ? "Heads" : "Tails");
                        }
                        std::cout << "\n";
                        break;
                    }
                    case MSG_TOSS_DICE: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "P" << (int)pl << " dice: ";
                        for (int i = 0; i < cnt; i++) {
                            uint8_t r = BufferIO::Read<uint8_t>(pbuf);
                            if (i > 0) std::cout << ", ";
                            std::cout << (int)r;
                        }
                        std::cout << "\n";
                        break;
                    }
                    case MSG_HAND_RES: {
                        pbuf += 1;
                        break;
                    }
                    case MSG_CONFIRM_DECKTOP: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "P" << (int)pl << " confirms deck top: ";
                        for (int i = 0; i < cnt; i++) {
                            uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                            if (i > 0) std::cout << ", ";
                            std::cout << get_card_name(code);
                        }
                        std::cout << "\n";
                        break;
                    }
                    case MSG_CONFIRM_EXTRATOP: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "P" << (int)pl << " confirms extra top: ";
                        for (int i = 0; i < cnt; i++) {
                            uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                            if (i > 0) std::cout << ", ";
                            std::cout << get_card_name(code);
                        }
                        std::cout << "\n";
                        break;
                    }
                    case MSG_CONFIRM_CARDS: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t skip = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        (void)skip;
                        std::cout << "P" << (int)pl << " confirms: ";
                        for (int i = 0; i < cnt; i++) {
                            uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                            uint8_t ctrl = BufferIO::Read<uint8_t>(pbuf);
                            uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                            uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                            (void)ctrl;(void)loc;(void)seq;
                            if (i > 0) std::cout << ", ";
                            std::cout << get_card_name(code);
                        }
                        std::cout << "\n";
                        break;
                    }
                    case MSG_RANDOM_SELECTED: {
                        // payload: player(1) + count(1) + count x info_location(4)
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "P" << (int)pl << " random selected: ";
                        for (int i = 0; i < cnt; i++) {
                            uint32_t il = BufferIO::Read<uint32_t>(pbuf);
                            if (i > 0) std::cout << ", ";
                            std::cout << location_name((il >> 8) & 0xff) << "[" << ((il >> 16) & 0xff) << "]";
                        }
                        std::cout << "\n";
                        break;
                    }
                    case MSG_CARD_SELECTED: {
                        // payload: player(1) + count(1) + count x code(4)
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        for (int i = 0; i < cnt; i++) BufferIO::Read<uint32_t>(pbuf);
                        std::cout << "P" << (int)pl << " selected " << (int)cnt << " card(s).\n";
                        break;
                    }
                    case MSG_SWAP_GRAVE_DECK: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "P" << (int)pl << " swapped grave and deck.\n";
                        break;
                    }
                    case MSG_REVERSE_DECK: {
                        std::cout << "Deck reversed.\n";
                        break;
                    }
                    case MSG_DECK_TOP: {
                        // payload: player(1) + count(1) + code(4)
                        pbuf += 6;
                        break;
                    }
                    case MSG_SHUFFLE_SET_CARD: {
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        // Payload: cnt info_location (4 bytes) + cnt more 4-byte fields.
                        for (int i = 0; i < cnt; i++) BufferIO::Read<uint32_t>(pbuf);
                        for (int i = 0; i < cnt; i++) BufferIO::Read<uint32_t>(pbuf);
                        std::cout << "Shuffled set cards in " << location_name(loc) << " (" << (int)cnt << ")\n";
                        break;
                    }
                    case MSG_MISSED_EFFECT: {
                        // payload: info_location(4) + code(4)
                        uint32_t il = BufferIO::Read<uint32_t>(pbuf);
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        (void)il;
                        std::cout << "Missed effect: " << get_card_name(code) << "\n";
                        break;
                    }
                    case MSG_TAG_SWAP: {
                        // payload: player(1) + main(1) + extra(1) + extra_p(1) +
                        // hand(1) + deck_top_code(4) + hand*code(4) + extra*code(4)
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        (void)pl;
                        uint8_t main_cnt = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t extra_cnt = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t extra_p_cnt = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t hand_cnt = BufferIO::Read<uint8_t>(pbuf);
                        (void)main_cnt;(void)extra_p_cnt;
                        pbuf += 4 + hand_cnt * 4 + extra_cnt * 4;
                        std::cout << "Tag swap.\n";
                        break;
                    }
                    case MSG_RELOAD_FIELD: {
                        std::cout << "Field reloaded.\n";
                        break;
                    }
                    case MSG_CARD_HINT: {
                        // payload: info_location(4) + hint_type(1) + value(4);
                        // no card code in the message (it is packed in the
                        // location's low bytes only for the controller)
                        uint32_t il = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t hint_type = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t hint_val = BufferIO::Read<uint32_t>(pbuf);
                        (void)il;(void)hint_type;(void)hint_val;
                        break;
                    }
                    case MSG_HINT: {
                        uint8_t hint_type = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t hint_val = BufferIO::Read<uint32_t>(pbuf);
                        (void)hint_type;(void)pl;(void)hint_val;
                        // Too verbose to narrate every hint
                        break;
                    }
                    case MSG_PLAYER_HINT: {
                        // payload: hint_type(1) + player(1) + value(4)
                        pbuf += 6;
                        break;
                    }
                    case MSG_SHOW_HINT: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t hint_val = BufferIO::Read<uint32_t>(pbuf);
                        (void)pl;(void)hint_val;
                        break;
                    }
                    default: {
                        // Unknown message - skip remaining buffer to avoid infinite loop
                        pbuf = msg_buffer + len;
                        break;
                    }
                }
            }
        }
    }
}

// Client-side field model: per (player, location, sequence) card.
// Populated from MSG_UPDATE_DATA / MSG_UPDATE_CARD / MSG_MOVE received from
// the server; used only for rendering (no local ocgcore).
struct CliCard {
    uint32_t code = 0;
    uint8_t pos = 0;
    int32_t atk = 0, def = 0;
    uint32_t level = 0, rank = 0;
    bool present = false;
};

// Network client field model (declared here so mcp_query_pos can fall back to
// it when no in-process engine exists; populated by cli_handle_msg).
static std::map<std::pair<int,int>, std::vector<CliCard>> cli_field;

// Position byte of the card at (player, loc, seq), 0 if none/not found.
uint8_t mcp_query_pos(int player, uint32_t loc, int seq) {
    if (global_pduel == 0) {
        // Network client mode: no in-process engine. Position comes from the
        // client-side model (may be stale but avoids a NULL-pduel crash).
        auto it = cli_field.find({player, (int)(loc & 0xff)});
        if (it != cli_field.end() && (int)it->second.size() > seq && it->second[seq].present)
            return it->second[seq].pos;
        return 0;
    }
    uint8_t buffer[8192];
    int len = query_field_card(global_pduel, player, loc, QUERY_CODE | QUERY_POSITION, buffer, 0);
    if (len <= 0) return 0;
    uint8_t* p = buffer;
    while (p - buffer < len) {
        uint8_t* card_start = p;
        int32_t card_len = BufferIO::Read<int32_t>(p);
        if (card_len > 4) {
            uint8_t* card_p = p;
            int32_t flag = BufferIO::Read<int32_t>(card_p);
            uint32_t info = 0;
            if (flag & QUERY_CODE) BufferIO::Read<uint32_t>(card_p);
            if (flag & QUERY_POSITION) info = BufferIO::Read<uint32_t>(card_p);
            if (((info >> 16) & 0xff) == (uint32_t)seq)
                return (uint8_t)((info >> 24) & 0xff);
        }
        p = card_start + card_len;
    }
    return 0;
}

// Location + state disambiguation for a card in a choice list, e.g.
// " @MZone[2] face-up", " @Hand[1]", " @Removed face-down", " @Overlay[1]".
// ctrl/loc/seq come from the choice message itself; position is queried.
// seq is shown for indexed zones (MZone/SZone/FZone/PZone/Overlay) and Hand,
// since duplicate cards there need disambiguation (e.g. two copies in hand).
std::string mcp_choice_suffix(int ctrl, uint32_t loc, int seq) {
    std::ostringstream out;
    out << " @" << location_name(loc);
    bool zoned = (loc & 0xff) == LOCATION_MZONE || (loc & 0xff) == LOCATION_SZONE ||
                 (loc & 0xff) == LOCATION_FZONE || (loc & 0xff) == LOCATION_PZONE;
    uint8_t base = loc & 0xff;
    if (zoned || base == LOCATION_OVERLAY || base == LOCATION_HAND ||
        base == LOCATION_EXTRA || base == LOCATION_DECK || base == LOCATION_GRAVE ||
        base == LOCATION_REMOVED) out << "[" << seq << "]";
    if (base == LOCATION_MZONE || base == LOCATION_SZONE || base == LOCATION_REMOVED) {
        uint8_t pos = mcp_query_pos(ctrl, loc, seq);
        if (pos) {
            if (base == LOCATION_MZONE) out << " " << pos_name(pos);
            else if (base == LOCATION_SZONE) out << " " << st_pos_label(pos);
            else out << ((pos & 0x1) ? " face-up" : " face-down");
        }
    }
    return out.str();
}

// Build the choice-list text for a pending prompt (from stored data).
// Returns e.g. "0: Summon X @Hand\n1: Go to Battle Phase\n".
std::string mcp_build_choices() {
    const auto& pp = mcp_pending_prompt;
    if (pp.msg_data.empty()) return "";
    std::ostringstream out;
    uint8_t* p = const_cast<uint8_t*>(pp.msg_data.data());
    uint8_t* end = p + pp.msg_data.size();
    int type = pp.msg_type;
    int idx = 0;

    // Helper reads (bounds-checked on stored buffer)
    auto rd8 = [&]() -> uint8_t { if (p < end) return *p++; return 0; };
    auto rd16 = [&]() -> uint16_t { uint16_t v=0; if (p+2<=end) std::memcpy(&v,p,2); p+=2; return v; };
    auto rd32 = [&]() -> uint32_t { uint32_t v=0; if (p+4<=end) std::memcpy(&v,p,4); p+=4; return v; };
    if (type == MSG_SELECT_IDLECMD) {
        rd8(); // skip player byte
        uint8_t sm = rd8();
        for (int i=0;i<sm;i++){ uint32_t c=rd32(); int ctrl=rd8(); uint8_t loc=rd8(); int seq=rd8(); out << idx++ << ": Summon " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << "\n"; }
        uint8_t sp = rd8();
        for (int i=0;i<sp;i++){ uint32_t c=rd32(); int ctrl=rd8(); uint8_t loc=rd8(); int seq=rd8(); out << idx++ << ": Special Summon " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << "\n"; }
        uint8_t rp = rd8();
        for (int i=0;i<rp;i++){ uint32_t c=rd32(); int ctrl=rd8(); uint8_t loc=rd8(); int seq=rd8(); out << idx++ << ": Reposition " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << "\n"; }
        uint8_t ms = rd8();
        for (int i=0;i<ms;i++){ uint32_t c=rd32(); int ctrl=rd8(); uint8_t loc=rd8(); int seq=rd8(); out << idx++ << ": Set " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << " (monster)\n"; }
        uint8_t ss = rd8();
        for (int i=0;i<ss;i++){ uint32_t c=rd32(); int ctrl=rd8(); uint8_t loc=rd8(); int seq=rd8(); out << idx++ << ": Set " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << " (spell/trap)\n"; }
        uint8_t ac = rd8();
        for (int i=0;i<ac;i++){ uint32_t c=rd32(); int ctrl=rd8(); uint8_t loc=rd8(); int seq=rd8(); rd32(); out << idx++ << ": Activate " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << "\n"; }
        uint8_t bp = rd8();
        uint8_t ep = rd8();
        rd8(); // shuffle
        if (bp) out << idx++ << ": Go to Battle Phase\n";
        if (ep) out << idx++ << ": Go to End Phase\n";
    } else if (type == MSG_SELECT_BATTLECMD) {
        rd8(); // skip player byte
        uint8_t ac = rd8();
        for (int i=0;i<ac;i++){ uint32_t c=rd32(); int ctrl=rd8(); uint8_t loc=rd8(); int seq=rd8(); rd32(); out << idx++ << ": Activate " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << "\n"; }
        uint8_t at = rd8();
        for (int i=0;i<at;i++){ uint32_t c=rd32(); int ctrl=rd8(); uint8_t loc=rd8(); int seq=rd8(); rd8(); out << idx++ << ": Attack with " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << "\n"; }
        uint8_t m2 = rd8();
        uint8_t ep = rd8();
        if (m2) out << idx++ << ": Go to Main Phase 2\n";
        if (ep) out << idx++ << ": Go to End Phase\n";
    } else if (type == MSG_SELECT_OPTION) {
        rd8(); // skip player byte
        uint8_t cnt = rd8();
        for (int i=0;i<cnt;i++){ int32_t o = (int32_t)rd32(); out << idx++ << ": " << get_desc_string((uint32_t)o) << "\n"; }
    } else if (type == MSG_SELECT_CARD || type == MSG_SELECT_TRIBUTE || type == MSG_SELECT_UNSELECT_CARD) {
        rd8(); // skip player byte
        rd8(); // cancelable/finishable
        if (type == MSG_SELECT_UNSELECT_CARD) rd8(); // cancelable
        int min = rd8();
        int max = rd8();
        int cnt = rd8();
        for (int i=0;i<cnt;i++){
            uint32_t c = rd32();
            uint32_t si = rd32();  // select_info: ctrl + loc<<8 + seq<<16 + pos<<24
            int ctrl = si & 0xff;
            uint32_t loc = (si >> 8) & 0xff;
            int seq = (si >> 16) & 0xff;
            out << idx++ << ": " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << "\n";
        }
        if (type == MSG_SELECT_UNSELECT_CARD) {
            int scnt = rd8();
            for (int i=0;i<scnt;i++){ rd32(); rd32(); }
        }
        out << "  (min " << min << ", max " << max << ")" << "\n";
    } else if (type == MSG_SELECT_CHAIN) {
        rd8(); // skip player byte
        uint8_t cnt = rd8(); // chain_count
        uint8_t spe = rd8();
        (void)spe;
        rd32(); rd32(); // hint0, hint1
        for (int i=0;i<cnt;i++){
            rd8(); // edesc
            uint8_t forced = rd8();
            uint32_t c = rd32();
            uint32_t il = rd32();  // info_location: ctrl + loc<<8 + seq<<16 + pos<<24
            uint32_t desc = rd32();
            int ctrl = il & 0xff;
            uint32_t loc = (il >> 8) & 0xff;
            int seq = (il >> 16) & 0xff;
            out << idx++ << ": " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq);
            if (forced) out << " (forced)";
            out << " [" << get_desc_string(desc) << "]\n";
        }
        out << "  [pass: -1]\n";
    } else if (type == MSG_ROCK_PAPER_SCISSORS) {
        rd8(); // skip player byte
        out << "0: Rock\n1: Paper\n2: Scissors\n";
    } else if (type == MSG_SELECT_POSITION) {
        rd8(); // skip player byte
        rd32(); // code
        uint8_t pos = rd8();
        if (pos & 0x1) out << idx++ << ": Face-up Attack\n";
        if (pos & 0x2) out << idx++ << ": Face-down Attack\n";
        if (pos & 0x4) out << idx++ << ": Face-up Defense\n";
        if (pos & 0x8) out << idx++ << ": Face-down Defense\n";
    } else if (type == MSG_SELECT_PLACE || type == MSG_SELECT_DISFIELD) {
        int chooser = rd8(); // player who picks
        rd8(); // count
        uint32_t forbidden = rd32();
        uint32_t sel = ~forbidden;
        int opp = 1 - chooser;
        for (int i=0;i<7;i++) if (sel & (1u<<i)) out << idx++ << ": P" << chooser << " MZone["<<i<<"]\n";
        for (int i=0;i<8;i++) if (sel & (0x100u<<i)) out << idx++ << ": P" << chooser << " SZone["<<i<<"]\n";
        for (int i=0;i<7;i++) if (sel & (0x10000u<<i)) out << idx++ << ": P" << opp << " MZone["<<i<<"]\n";
        for (int i=0;i<8;i++) if (sel & (0x1000000u<<i)) out << idx++ << ": P" << opp << " SZone["<<i<<"]\n";
    } else if (type == MSG_SELECT_SUM) {
        rd8(); // mode (0 or 1)
        rd8(); // skip player byte
        rd32(); // acc
        rd8(); rd8(); // min, max
        int mcnt = rd8(); // must_count
        for (int i=0;i<mcnt;i++){ uint32_t c=rd32(); int ctrl=rd8(); uint8_t loc=rd8(); int seq=rd8(); rd32(); out << idx++ << ": " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << " (must)\n"; }
        int scnt = rd8(); // select_count
        for (int i=0;i<scnt;i++){ uint32_t c=rd32(); int ctrl=rd8(); uint8_t loc=rd8(); int seq=rd8(); rd32(); out << idx++ << ": " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << "\n"; }
    } else if (type == MSG_SELECT_YESNO || type == MSG_SELECT_EFFECTYN) {
        rd8(); // skip player byte
        std::string q;
        if (type == MSG_SELECT_EFFECTYN) {
            uint32_t c = rd32();
            rd32(); // info_location (ctrl+loc<<8+seq<<16+pos<<24)
            q = get_desc_string(rd32());
            out << "(Activate " << get_card_name(c) << "?) " << q << "\n";
        } else {
            q = get_desc_string(rd32());
            out << "(" << q << ")\n";
        }
        out << "0: No\n1: Yes\n";
    } else if (type == MSG_ANNOUNCE_RACE) {
        rd8(); // skip player byte
        uint32_t avail = rd32();
        for (uint32_t r=0;r<26;r++) if (avail & (1u<<r)) out << idx++ << ": " << race_name(1u<<r) << "\n";
    } else if (type == MSG_ANNOUNCE_ATTRIB) {
        rd8(); // skip player byte
        uint32_t avail = rd32();
        for (uint32_t a=0;a<7;a++) if (avail & (1u<<a)) out << idx++ << ": " << attribute_name(1u<<a) << "\n";
    } else if (type == MSG_ANNOUNCE_CARD) {
        rd8(); // skip player byte
        uint8_t cnt = rd8();
        for (int i=0;i<cnt;i++){ uint32_t c=rd32(); out << idx++ << ": " << get_card_name(c) << "\n"; }
    } else if (type == MSG_ANNOUNCE_NUMBER) {
        rd8(); // skip player byte
        uint8_t cnt = rd8();
        for (int i=0;i<cnt;i++){ uint32_t v=rd32(); out << idx++ << ": " << v << "\n"; }
    } else if (type == MSG_SORT_CARD) {
        rd8(); // skip player byte
        uint8_t cnt = rd8();
        for (int i=0;i<cnt;i++){ uint32_t c=rd32(); out << idx++ << ": " << get_card_name(c) << "\n"; }
        out << "  (respond in sort order)\n";
    } else if (type == MSG_SELECT_COUNTER) {
        rd8(); // skip player byte
        rd16(); // countertype
        rd16(); // count (number of counters to distribute)
        uint8_t cnt = rd8();  // card count
        for (int i=0;i<cnt;i++){ uint32_t c=rd32(); int ctrl=rd8(); uint8_t loc=rd8(); int seq=rd8(); rd16(); out << idx++ << ": Put counters on " << get_card_name(c) << mcp_choice_suffix(ctrl, loc, seq) << "\n"; }
    } else {
        out << "(prompt type " << type << " - no options)\n" << "  [pass: -1]\n";
    }
    return out.str();
}


// Send a choice to the engine based on the pending prompt.
// choice_idx: 0-based index into the options, or -1 to cancel/pass.
// choice_indices: for multi-select prompts, the ordered list of indices.
// Called after mcp_run_until_pause() returns 0.
void mcp_send_choice(int choice_idx, const std::vector<int>& choice_indices) {
    intptr_t pduel = global_pduel;
    // Network clients have no in-process engine (global_pduel == 0); they still
    // need the response bytes built for CTOS_RESPONSE.
    if (pduel == 0 && !net_client_mode) return;
    int type = mcp_pending_prompt.msg_type;

    // Re-read stored data for response construction
    uint8_t* p = const_cast<uint8_t*>(mcp_pending_prompt.msg_data.data());
    uint8_t* end = p + mcp_pending_prompt.msg_data.size();

    auto rd8  = [&]() -> uint8_t  { if (p < end) return *p++; return 0; };
    auto rd16 = [&]() -> uint16_t { uint16_t v=0; if (p+2<=end) std::memcpy(&v,p,2); p+=2; return v; };
    auto rd32 = [&]() -> uint32_t { uint32_t v=0; if (p+4<=end) std::memcpy(&v,p,4); p+=4; return v; };

    auto send_int = [&](int32_t val) {
        if (net_client_mode) {
            // network client: pack the int as 4 little-endian bytes
            net_response_buf.clear();
            net_response_buf.resize(4);
            std::memcpy(net_response_buf.data(), &val, 4);
            return;
        }
        mcp_last_response.valid = true;
        mcp_last_response.is_buffer = false;
        mcp_last_response.int_val = val;
        set_responsei(pduel, val);
    };
    auto send_buf = [&](const std::vector<uint8_t>& buf) {
        if (net_client_mode) {
            net_response_buf = buf;
            return;
        }
        mcp_last_response.valid = true;
        mcp_last_response.is_buffer = true;
        mcp_last_response.buf = buf;
        set_responseb(pduel, const_cast<uint8_t*>(buf.data()));
    };

    int idx = (choice_idx >= 0) ? choice_idx : -1;

    switch (type) {
        case MSG_SELECT_IDLECMD: {
            // Parse all categories to find type and sub-index
            rd8(); // skip player byte
            uint8_t sm = rd8();
            for (int i=0;i<sm;i++){ rd32(); rd8();rd8();rd8(); }
            uint8_t sp = rd8();
            for (int i=0;i<sp;i++){ rd32(); rd8();rd8();rd8(); }
            uint8_t rp = rd8();
            for (int i=0;i<rp;i++){ rd32(); rd8();rd8();rd8(); }
            uint8_t ms = rd8();
            for (int i=0;i<ms;i++){ rd32(); rd8();rd8();rd8(); }
            uint8_t ss = rd8();
            for (int i=0;i<ss;i++){ rd32(); rd8();rd8();rd8(); }
            uint8_t ac = rd8();
            for (int i=0;i<ac;i++){ rd32(); rd8();rd8();rd8(); rd32(); }
            uint8_t bp = rd8(); uint8_t ep = rd8(); rd8();

            int cat = 0, sub = idx;
            if (idx >= 0) {
                if (idx < sm) { cat = 0; sub = idx; }
                else if (idx < sm+sp) { cat = 1; sub = idx - sm; }
                else if (idx < sm+sp+rp) { cat = 2; sub = idx - sm - sp; }
                else if (idx < sm+sp+rp+ms) { cat = 3; sub = idx - sm - sp - rp; }
                else if (idx < sm+sp+rp+ms+ss) { cat = 4; sub = idx - sm - sp - rp - ms; }
                else if (idx < sm+sp+rp+ms+ss+ac) { cat = 5; sub = idx - sm - sp - rp - ms - ss; }
                else if (bp && idx == sm+sp+rp+ms+ss+ac) { cat = 6; sub = 0; }
                else if (ep && idx == sm+sp+rp+ms+ss+ac+(bp?1:0)) { cat = 7; sub = 0; }
            } else {
                // Pass (-1): advance to the next phase. Prefer the End Phase
                // (cat 7), else Battle Phase (cat 6) in Main1. Without this the
                // client would send (0xFFFF<<16) which the core rejects with
                // MSG_RETRY, deadlocking a "pass only" idle prompt.
                if (ep) { cat = 7; sub = 0; }
                else if (bp) { cat = 6; sub = 0; }
            }
            send_int((sub << 16) | cat);
            break;
        }
        case MSG_SELECT_BATTLECMD: {
            rd8(); // skip player byte
            uint8_t ac = rd8();
            for (int i=0;i<ac;i++){ rd32(); rd8();rd8();rd8(); rd32(); }
            uint8_t at = rd8();
            for (int i=0;i<at;i++){ rd32(); rd8();rd8();rd8(); rd8(); }
            uint8_t m2 = rd8(); uint8_t ep = rd8();

            int cat = 0, sub = idx;
            if (idx >= 0) {
                if (idx < ac) { cat = 0; sub = idx; }
                else if (idx < ac+at) { cat = 1; sub = idx - ac; }
                else if (m2 && idx == ac+at) { cat = 2; sub = 0; }
                else if (ep && idx == ac+at+(m2?1:0)) { cat = 3; sub = 0; }
            } else {
                // Pass (-1): go to Main Phase 2 (cat 2) if allowed, else End.
                if (m2) { cat = 2; sub = 0; }
                else if (ep) { cat = 3; sub = 0; }
            }
            send_int((sub << 16) | cat);
            break;
        }
        case MSG_SELECT_YESNO:
        case MSG_SELECT_EFFECTYN:
            rd8(); // skip player byte
            send_int(idx >= 0 ? idx : 0);
            break;
        case MSG_SELECT_OPTION:
            rd8(); // skip player byte
            send_int(idx >= 0 ? idx : -1);
            break;
        case MSG_SELECT_CHAIN:
            rd8(); // skip player byte
            send_int(idx);  // -1 to pass, 0..N to chain
            break;
        case MSG_SELECT_CARD:
        case MSG_SELECT_TRIBUTE: {
            rd8(); // skip player byte
            rd8(); // cancelable
            uint8_t min = rd8(); uint8_t max = rd8();
            (void)min; (void)max;
            uint8_t cnt = rd8();
            for (int i=0;i<cnt;i++){ rd32(); rd8();rd8();rd8();rd8(); }
            if (idx == -1 || choice_idx == -1) {
                send_int(-1);
            } else if (!choice_indices.empty()) {
                std::vector<uint8_t> buf(1 + choice_indices.size());
                buf[0] = (uint8_t)choice_indices.size();
                for (size_t i=0;i<choice_indices.size();i++) buf[1+i] = (uint8_t)choice_indices[i];
                send_buf(buf);
            } else {
                std::vector<uint8_t> buf(2);
                buf[0] = 1; buf[1] = (uint8_t)(idx >= 0 ? idx : 0);
                send_buf(buf);
            }
            break;
        }
        case MSG_SELECT_UNSELECT_CARD: {
            rd8(); // skip player byte
            rd8(); // finishable
            rd8(); // cancelable
            rd8(); rd8(); // min, max
            uint8_t cnt = rd8();
            for (int i=0;i<cnt;i++){ rd32(); rd8();rd8();rd8();rd8(); }
            // skip selected cards
            uint8_t scnt = rd8();
            for (int i=0;i<scnt;i++){ rd32(); rd8();rd8();rd8();rd8(); }
            if (idx == -1) {
                std::vector<uint8_t> buf(4, 0xff);
                send_buf(buf);
            } else {
                std::vector<uint8_t> buf(2);
                buf[0] = 1; buf[1] = (uint8_t)idx;
                send_buf(buf);
            }
            break;
        }
        case MSG_SELECT_PLACE:
        case MSG_SELECT_DISFIELD: {
            int chooser = rd8(); // player who picks
            rd8(); // count
            uint32_t forbidden = rd32();
            uint32_t sel = ~forbidden;
            int opp = 1 - chooser;
            // Build the same options list as mcp_build_choices
            struct PlaceOpt { uint8_t ctrl,loc,seq; };
            std::vector<PlaceOpt> opts;
            for (int i=0;i<7;i++) if (sel & (1u<<i)) opts.push_back({(uint8_t)chooser,(uint8_t)LOCATION_MZONE,(uint8_t)i});
            for (int i=0;i<8;i++) if (sel & (0x100u<<i)) opts.push_back({(uint8_t)chooser,(uint8_t)LOCATION_SZONE,(uint8_t)i});
            for (int i=0;i<7;i++) if (sel & (0x10000u<<i)) opts.push_back({(uint8_t)opp,(uint8_t)LOCATION_MZONE,(uint8_t)i});
            for (int i=0;i<8;i++) if (sel & (0x1000000u<<i)) opts.push_back({(uint8_t)opp,(uint8_t)LOCATION_SZONE,(uint8_t)i});
            uint8_t buf[3] = {0, (uint8_t)LOCATION_MZONE, 0};
            if (idx >= 0 && (size_t)idx < opts.size()) {
                buf[0] = opts[idx].ctrl;
                buf[1] = opts[idx].loc;
                buf[2] = opts[idx].seq;
            }
            send_buf({buf, buf+3});
            break;
        }
        case MSG_SELECT_POSITION: {
            rd8(); // skip player byte
            rd32(); // code
            uint8_t pos = rd8();
            // Build position list same as mcp_build_choices
            uint8_t positions[4] = {0};
            int pcount = 0;
            if (pos & 0x1) positions[pcount++] = POS_FACEUP_ATTACK;
            if (pos & 0x2) positions[pcount++] = POS_FACEDOWN_ATTACK;
            if (pos & 0x4) positions[pcount++] = POS_FACEUP_DEFENSE;
            if (pos & 0x8) positions[pcount++] = POS_FACEDOWN_DEFENSE;
            if (idx >= 0 && idx < pcount) {
                send_int(positions[idx]);
            } else {
                send_int(positions[0]);
            }
            break;
        }
        case MSG_SELECT_SUM: {
            rd8(); // mode
            rd8(); // player
            rd32(); // acc
            rd8(); rd8(); // min, max
            uint8_t mcnt = rd8();
            for (int i=0;i<mcnt;i++){ rd32(); rd8();rd8();rd8(); rd32(); }
            uint8_t scnt = rd8();
            for (int i=0;i<scnt;i++){ rd32(); rd8();rd8();rd8(); rd32(); }
            // bvalue[0] = total count, then indices (must first, then optional)
            if (idx == -1) {
                send_int(-1);
            } else {
                // Build response: total = mcnt + selected_optional_count
                // For optional cards, choice_indices are into the combined options list
                // Options list: must cards (0..mcnt-1), then selectable cards (mcnt..mcnt+scnt-1)
                // Engine indices: must cards 0..mcnt-1, selectable cards 0..scnt-1
                std::vector<uint8_t> buf;
                buf.push_back((uint8_t)(mcnt + (choice_indices.empty() ? 1 : choice_indices.size())));
                for (int i = 0; i < (int)mcnt; i++) buf.push_back((uint8_t)i);
                if (!choice_indices.empty()) {
                    for (int ci : choice_indices) {
                        int engine_idx = ci - mcnt;
                        if (engine_idx >= 0 && engine_idx < (int)scnt) buf.push_back((uint8_t)engine_idx);
                    }
                } else if (scnt > 0) {
                    buf.push_back(0);
                }
                send_buf(buf);
            }
            break;
        }
        case MSG_SELECT_COUNTER: {
            rd8(); // skip player byte
            rd16(); // countertype
            rd16(); // count
            uint8_t cnt = rd8();
            for (int i=0;i<(int)cnt;i++){ rd32(); rd8();rd8();rd8(); rd16(); }
            send_int(idx >= 0 ? idx : 0);
            break;
        }
        case MSG_SORT_CARD: {
            rd8(); // skip player byte
            uint8_t cnt = rd8();
            for (int i=0;i<cnt;i++){ rd32(); }
            if (!choice_indices.empty()) {
                uint32_t mask = 0;
                for (int i : choice_indices) mask |= (1u << i);
                send_int((int32_t)mask);
            } else if (idx >= 0) {
                send_int(1 << idx);
            } else {
                send_int(0);
            }
            break;
        }
        case MSG_ROCK_PAPER_SCISSORS:
            rd8(); // skip player byte
            send_int(idx >= 0 ? idx : 0);
            break;
        case MSG_ANNOUNCE_RACE: {
            rd8(); // skip player byte
            uint32_t avail = rd32();
            std::vector<uint32_t> races;
            for (uint32_t r=0;r<26;r++) if (avail & (1u<<r)) races.push_back(1u<<r);
            if (idx >= 0 && (size_t)idx < races.size()) send_int((int32_t)races[idx]);
            else send_int((int32_t)(races.empty() ? 0 : races[0]));
            break;
        }
        case MSG_ANNOUNCE_ATTRIB: {
            rd8(); // skip player byte
            uint32_t avail = rd32();
            std::vector<uint32_t> attrs;
            for (uint32_t a=0;a<7;a++) if (avail & (1u<<a)) attrs.push_back(1u<<a);
            if (idx >= 0 && (size_t)idx < attrs.size()) send_int((int32_t)attrs[idx]);
            else send_int((int32_t)(attrs.empty() ? 0 : attrs[0]));
            break;
        }
        case MSG_ANNOUNCE_CARD: {
            rd8(); // skip player byte
            uint8_t cnt = rd8();
            std::vector<uint32_t> cards;
            for (int i=0;i<cnt;i++) cards.push_back(rd32());
            if (idx >= 0 && (size_t)idx < cards.size()) send_int((int32_t)cards[idx]);
            else send_int((int32_t)(cards.empty() ? 0 : cards[0]));
            break;
        }
        case MSG_ANNOUNCE_NUMBER: {
            rd8(); // skip player byte
            uint8_t cnt = rd8();
            std::vector<uint32_t> nums;
            for (int i=0;i<cnt;i++) nums.push_back(rd32());
            if (idx >= 0 && (size_t)idx < nums.size()) send_int((int32_t)nums[idx]);
            else send_int((int32_t)(nums.empty() ? 0 : nums[0]));
            break;
        }
        case MSG_HAND_RES:
            send_int(idx >= 0 ? idx : 0);
            break;
        default:
            send_int(-1);
            break;
    }
}

// ======= MCP JSON-RPC 2.0 server loop =======
// Handles initialize, tools/list, and tools/call (ygo_single_mode, ygo_client, ygo_choose, ygo_card_search)

// Persistent deck paths for restarting games
static std::string mcp_deck0_path, mcp_deck1_path;

// ygo_card_search: query card database
static std::string mcp_tool_card_search(const nlohmann::json& params) {
    // Filters are ANDed; all params optional. Accept int or string for numeric fields.
    auto num_str = [](const nlohmann::json& v) -> std::string {
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number()) return std::to_string(v.get<int>());
        return "";
    };

    // Build filter conditions
    uint32_t id_filter = 0;
    bool has_id = false;
    if (params.contains("id") && params["id"].is_number()) {
        id_filter = (uint32_t)params["id"];
        has_id = true;
    }

    // Text filter: split by space, all tokens must appear in name+desc
    std::vector<std::string> text_tokens;
    bool has_text = false;
    if (params.contains("text")) {
        std::string t = num_str(params["text"]);
        std::string tok;
        for (char c : t) {
            if (c == ' ' || c == '\t') {
                if (!tok.empty()) { text_tokens.push_back(tok); tok.clear(); }
            } else {
                tok += (char)std::tolower((unsigned char)c);
            }
        }
        if (!tok.empty()) text_tokens.push_back(tok);
        has_text = !text_tokens.empty();
    }

    // Numeric filters: level, atk, def. Accept int or ">", ">=", "=", "<=", "<" prefixed string.
    auto parse_num_filter = [](const std::string& s, int& op, int& val) -> bool {
        if (s.empty()) return false;
        op = 0; val = 0;
        const char* c = s.c_str();
        if (*c == '>') {
            if (*(c+1) == '=') { op = 3; c += 2; } // >=
            else { op = 1; c++; } // >
        } else if (*c == '<') {
            if (*(c+1) == '=') { op = 4; c += 2; } // <=
            else { op = 2; c++; } // <
        } else if (*c == '=') {
            op = 0; c++;
        }
        val = (int)std::strtol(c, nullptr, 10);
        return true;
    };

    int lv_op = -1, lv_val = 0;
    int atk_op = -1, atk_val = 0;
    int def_op = -1, def_val = 0;
    if (params.contains("level")) parse_num_filter(num_str(params["level"]), lv_op, lv_val);
    if (params.contains("atk")) parse_num_filter(num_str(params["atk"]), atk_op, atk_val);
    if (params.contains("def")) parse_num_filter(num_str(params["def"]), def_op, def_val);

    // Type filter: "Normal|Monster" style string, or a raw type bitmask int.
    uint32_t type_filter = 0;
    bool has_type = false;
    if (params.contains("type")) {
        has_type = true;
        std::string t = num_str(params["type"]);
        // Parse type string like "Normal|Monster" or "Spell" into bitmask
        static const std::pair<const char*, uint32_t> type_map[] = {
            {"Monster", TYPE_MONSTER}, {"Spell", TYPE_SPELL}, {"Trap", TYPE_TRAP},
            {"Normal", TYPE_NORMAL}, {"Effect", TYPE_EFFECT}, {"Fusion", TYPE_FUSION},
            {"Ritual", TYPE_RITUAL}, {"Spirit", TYPE_SPIRIT}, {"Union", TYPE_UNION},
            {"Dual", TYPE_DUAL}, {"Tuner", TYPE_TUNER}, {"Synchro", TYPE_SYNCHRO},
            {"Token", TYPE_TOKEN}, {"QuickPlay", TYPE_QUICKPLAY}, {"Continuous", TYPE_CONTINUOUS},
            {"Equip", TYPE_EQUIP}, {"Field", TYPE_FIELD}, {"Counter", TYPE_COUNTER},
            {"Flip", TYPE_FLIP}, {"Toon", TYPE_TOON}, {"Xyz", TYPE_XYZ},
            {"Pendulum", TYPE_PENDULUM}, {"SPSummon", TYPE_SPSUMMON}, {"Link", TYPE_LINK},
        };
        std::string tok;
        for (char c : t) {
            if (c == '|') {
                if (!tok.empty()) {
                    for (auto& m : type_map) if (tok == m.first) type_filter |= m.second;
                    tok.clear();
                }
            } else {
                tok += c;
            }
        }
        if (!tok.empty()) {
            for (auto& m : type_map) if (tok == m.first) type_filter |= m.second;
        }
        // Also try numeric
        if (type_filter == 0) {
            type_filter = (uint32_t)std::strtoul(t.c_str(), nullptr, 0);
        }
    }

    // Race filter (OR with |)
    std::vector<uint32_t> race_filters;
    if (params.contains("race")) {
        std::string r = num_str(params["race"]);
        static const std::pair<const char*, uint32_t> race_map[] = {
            {"Warrior",RACE_WARRIOR},{"Spellcaster",RACE_SPELLCASTER},{"Fairy",RACE_FAIRY},
            {"Fiend",RACE_FIEND},{"Zombie",RACE_ZOMBIE},{"Machine",RACE_MACHINE},
            {"Aqua",RACE_AQUA},{"Pyro",RACE_PYRO},{"Rock",RACE_ROCK},
            {"WingedBeast",RACE_WINDBEAST},{"Winged Beast",RACE_WINDBEAST},
            {"Plant",RACE_PLANT},{"Insect",RACE_INSECT},{"Thunder",RACE_THUNDER},
            {"Dragon",RACE_DRAGON},{"Beast",RACE_BEAST},{"Beast-Warrior",RACE_BEASTWARRIOR},
            {"BeastWarrior",RACE_BEASTWARRIOR},{"Dinosaur",RACE_DINOSAUR},{"Fish",RACE_FISH},
            {"SeaSerpent",RACE_SEASERPENT},{"Sea Serpent",RACE_SEASERPENT},
            {"Reptile",RACE_REPTILE},{"Psychic",RACE_PSYCHO},{"Divine-Beast",RACE_DEVINE},
            {"DivineBeast",RACE_DEVINE},{"CreatorGod",RACE_CREATORGOD},{"Creator God",RACE_CREATORGOD},
            {"Wyrm",RACE_WYRM},{"Cyberse",RACE_CYBERSE},{"Illusion",RACE_ILLUSION},
        };
        std::string tok;
        for (char c : r) {
            if (c == '|') {
                if (!tok.empty()) {
                    for (auto& m : race_map) if (tok == m.first) race_filters.push_back(m.second);
                    tok.clear();
                }
            } else {
                tok += c;
            }
        }
        if (!tok.empty()) {
            for (auto& m : race_map) if (tok == m.first) race_filters.push_back(m.second);
        }
    }

    // Attribute filter (OR with |)
    std::vector<uint32_t> attr_filters;
    if (params.contains("attribute")) {
        std::string a = num_str(params["attribute"]);
        static const std::pair<const char*, uint32_t> attr_map[] = {
            {"Earth",ATTRIBUTE_EARTH},{"Water",ATTRIBUTE_WATER},{"Fire",ATTRIBUTE_FIRE},
            {"Wind",ATTRIBUTE_WIND},{"Light",ATTRIBUTE_LIGHT},{"Dark",ATTRIBUTE_DARK},
            {"Divine",ATTRIBUTE_DEVINE},
        };
        std::string tok;
        for (char c : a) {
            if (c == '|') {
                if (!tok.empty()) {
                    for (auto& m : attr_map) if (tok == m.first) attr_filters.push_back(m.second);
                    tok.clear();
                }
            } else {
                tok += c;
            }
        }
        if (!tok.empty()) {
            for (auto& m : attr_map) if (tok == m.first) attr_filters.push_back(m.second);
        }
    }

    // Scan all cards
    std::vector<uint32_t> results;
    std::unordered_set<uint32_t> seen_results;
    for (auto& [code, data] : card_datas) {
        if (seen_results.count(code)) continue;
        if (has_id && code != id_filter) continue;
        // Text filter
        if (has_text) {
            std::string name = card_names.count(code) ? card_names[code] : "";
            std::string desc = card_descs.count(code) ? card_descs[code] : "";
            std::string combined;
            for (char c : name) combined += (char)std::tolower((unsigned char)c);
            for (char c : desc) combined += (char)std::tolower((unsigned char)c);
            bool all_found = true;
            for (auto& tok : text_tokens) {
                if (combined.find(tok) == std::string::npos) { all_found = false; break; }
            }
            if (!all_found) continue;
        }
        // Level filter
        if (lv_op >= 0) {
            int lv = data.level & 0xff;
            if (lv_op == 0 && lv != lv_val) continue;
            if (lv_op == 1 && lv <= lv_val) continue;
            if (lv_op == 2 && lv >= lv_val) continue;
            if (lv_op == 3 && lv < lv_val) continue;
            if (lv_op == 4 && lv > lv_val) continue;
        }
        // ATK filter
        if (atk_op >= 0) {
            int a = data.attack;
            if (atk_op == 0 && a != atk_val) continue;
            if (atk_op == 1 && a <= atk_val) continue;
            if (atk_op == 2 && a >= atk_val) continue;
            if (atk_op == 3 && a < atk_val) continue;
            if (atk_op == 4 && a > atk_val) continue;
        }
        // DEF filter
        if (def_op >= 0) {
            int d = data.defense;
            if (def_op == 0 && d != def_val) continue;
            if (def_op == 1 && d <= def_val) continue;
            if (def_op == 2 && d >= def_val) continue;
            if (def_op == 3 && d < def_val) continue;
            if (def_op == 4 && d > def_val) continue;
        }
        // Type filter
        if (has_type && (data.type & type_filter) != type_filter) continue;
        // Race filter
        if (!race_filters.empty()) {
            bool match = false;
            for (uint32_t r : race_filters) if (data.race == r) { match = true; break; }
            if (!match) continue;
        }
        // Attribute filter
        if (!attr_filters.empty()) {
            bool match = false;
            for (uint32_t a : attr_filters) if (data.attribute == a) { match = true; break; }
            if (!match) continue;
        }
        seen_results.insert(code);
        results.push_back(code);
    }

    // Cap at 100
    if (results.size() > 100) results.resize(100);

    std::ostringstream out;
    out << "Found " << results.size() << " cards:\n";
    for (uint32_t code : results) {
        out << card_detail_line(code) << "\n";
    }
    return out.str();
}

// Register engine callbacks (card data reader, script reader that resolves
// bare names against script/ while passing absolute paths through, and the
// error-message logger). Shared by the solo (MCP) and auto-play paths.
static void setup_engine_callbacks() {
    set_card_reader([](uint32_t code, card_data* data) -> uint32_t {
        auto it = card_datas.find(code);
        if (it != card_datas.end()) {
            *data = it->second;
            return 0;
        }
        data->clear();
        return 0;
    });
    set_script_reader([](const char* script_name, int* len) -> byte* {
        std::string requested = script_name ? script_name : "";
        std::string path;
        if (!requested.empty() && (requested[0] == '/' || requested.rfind("./", 0) == 0
                                   || requested.rfind("../", 0) == 0)) {
            path = requested;            // absolute / explicit relative paths pass through
        } else if (requested.rfind("script/", 0) == 0) {
            path = "./" + requested;
        } else {
            path = "./script/" + requested;   // bare names resolve against script/
        }
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            *len = 0;
            return nullptr;
        }
        *len = file.tellg();
        file.seekg(0, std::ios::beg);
        char* buf = new char[*len + 1];
        file.read(buf, *len);
        buf[*len] = '\0';
        return reinterpret_cast<byte*>(buf);
    });
    set_message_handler([](intptr_t pduel, uint32_t msg_type) -> uint32_t {
        (void)msg_type;
        char log_buf[2048] = {};
        get_log_message(pduel, log_buf);
        if (log_buf[0] != '\0') {
            print_core_error(log_buf);
        }
        return 0;
    });
}

static void mcp_setup_engine() {
    setup_engine_callbacks();
}

// Initialize a duel (reused by ygo_solo and the initial setup). The optional
// setup_lua runs in the duel's lua context between deck creation and
// start_duel (puzzle support): the script can call Duel.* helpers
// (e.g. Duel.SetLP(1, 100)) to build a puzzle state.
static bool mcp_init_duel(const std::string& deck0_path, const std::string& deck1_path,
                          int start_lp, int start_hand, int draw_count, int rule,
                          const std::string& setup_lua = "") {
    if (global_pduel != 0) {
        end_duel(global_pduel);
        global_pduel = 0;
    }

    // Load decks
    Deck decks[2];
    decks[0] = load_deck(deck0_path);
    decks[1] = load_deck(deck1_path);
    if (decks[0].main.empty() || decks[1].main.empty()) return false;

    // Shuffle
    std::mt19937 rng_shuffle;
    std::random_device rd_shuffle;
    rng_shuffle.seed(rd_shuffle());
    for (int i = 0; i < 2; i++) {
        std::shuffle(decks[i].main.begin(), decks[i].main.end(), rng_shuffle);
        std::shuffle(decks[i].extra.begin(), decks[i].extra.end(), rng_shuffle);
    }

    // Seeds
    uint32_t seed[SEED_COUNT];
    std::random_device rd;
    for (int i = 0; i < SEED_COUNT; i++) seed[i] = rd();

    mcp_setup_engine();
    intptr_t pduel = create_duel_v2(seed);
    global_pduel = pduel;
    set_player_info(pduel, 0, start_lp, start_hand, draw_count);
    set_player_info(pduel, 1, start_lp, start_hand, draw_count);

    for (int i = 0; i < 2; i++) {
        for (size_t j = 0; j < decks[i].main.size(); j++) {
            new_card(pduel, decks[i].main[j], i, i, LOCATION_DECK, decks[i].main.size() - 1 - j, POS_FACEDOWN_DEFENSE);
        }
        for (size_t j = 0; j < decks[i].extra.size(); j++) {
            new_card(pduel, decks[i].extra[j], i, i, LOCATION_EXTRA, j, POS_FACEDOWN_DEFENSE);
        }
    }

    if (!setup_lua.empty()) {
        if (preload_script(pduel, setup_lua.c_str()) == OPERATION_FAIL) {
            char log_buf[2048] = {};
            get_log_message(pduel, log_buf);
            fprintf(stderr, "puzzle setup script failed: %s (%s)\n", setup_lua.c_str(), log_buf);
        }
    }
    start_duel(pduel, DUEL_SIMPLE_AI | ((uint32_t)rule << 16));
    mcp_engine_buffer.resize(SIZE_MESSAGE_BUFFER);
    mcp_reset_game_state();
    return true;
}

// ygo_single_mode tool: init new game (both players in-process, unfiltered), run until choice
static std::string mcp_tool_ygo_single_mode(const nlohmann::json& params) {
    // Parse params (each optional with a sensible default)
    std::string deck0 = params.value("deck0", mcp_deck0_path);
    std::string deck1 = params.value("deck1", mcp_deck1_path);
    int lp = params.value("lp", 8000);
    int start_hand = params.value("start_hand", 5);
    int draw_count = params.value("draw_count", 1);
    int rule = params.value("rule", CURRENT_RULE);

    if (!mcp_init_duel(deck0, deck1, lp, start_hand, draw_count, rule)) {
        return "Error: Failed to load decks";
    }

    mcp_begin_capture();
    int ret = mcp_run_until_pause();
    std::string narration = mcp_take_capture();
    mcp_end_capture();

    if (ret == 1) {
        // Game over
        std::string result = mcp_format_output(narration, "");
        result += "Game Over: Player " + std::to_string(mcp_winner) + " wins!";
        end_duel(global_pduel);
        global_pduel = 0;
        return result;
    }
    if (ret == -1) return "Error: Game not started";

    std::string choices = mcp_build_choices();
    return mcp_format_output(narration, choices);
}

// Network client field model + state (definitions moved here so the MCP tools
// can use them; the rendering functions themselves live before main()).
static int cli_turn = 1;
static int cli_phase = 0x04;
static int cli_lp[2] = {8000, 8000};
static bool cli_has_prompt = false;
static bool cli_game_over = false;
static int cli_winner = 0;
// Room-lobby state (network): 0=lobby, 1=hand(RPS), 2=tp, 3=dueling.
static int cli_stage = 0;
static bool cli_is_host = false;
static bool cli_ready[2] = {false, false};
static std::string cli_names[2];
static int cli_pending_hand = 0;   // nonzero while STOC_SELECT_HAND outstanding
static int cli_pending_tp = 0;     // nonzero while STOC_SELECT_TP outstanding
static uint32_t cli_last_err_msg = 0;   // last STOC_ERROR_MSG fields
static uint32_t cli_last_err_code = 0;
static bool cli_match_mode = false;     // room is a BO3 match (host_info.mode==1)
static bool cli_match_over = false;     // match decided (STOC_DUEL_END in a match)
static int cli_read_until_prompt(int timeout_ms);
static int cli_wait_for_prompt(int timeout_ms, int max_idle);
static std::string cli_render_field();
static std::string cli_err_reason(uint32_t msg, uint32_t code);
static void cli_net_reset();
static std::string mcp_tool_ygo_client(const nlohmann::json& params);
static std::string mcp_tool_ygo_replay(const nlohmann::json& params);
static std::string mcp_tool_ygo_puzzle(const nlohmann::json& params);
static std::string cli_lobby_choices();
static std::vector<uint8_t> cli_build_deck_payload(const std::vector<uint32_t>& codes);

// ygo_choose tool: send choice, continue game (solo in-process or network client)
static std::string mcp_tool_ygo_choose(const nlohmann::json& params) {
    int choice_idx = -1;
    std::vector<int> choice_indices;
    if (params.contains("id") && params["id"].is_number()) choice_idx = params["id"];
    if (params.contains("indices") && params["indices"].is_array()) {
        for (const auto& v : params["indices"])
            if (v.is_number()) choice_indices.push_back(v);
    }

    // Main menu: no active game, no connection. Choices mirror the GUI main menu
    // (solo / network / replay / puzzle). The chosen action consumes the params.
    if (global_pduel == 0 && !(net_client_connected && net_client_fd >= 0)) {
        if (choice_idx == 0) return mcp_tool_ygo_single_mode(params);
        if (choice_idx == 1) return mcp_tool_ygo_client(params);
        if (choice_idx == 2) return mcp_tool_ygo_replay(params);
        if (choice_idx == 3) return mcp_tool_ygo_puzzle(params);
        std::string menu =
            "Main menu:\n"
            "  0. solo game (deck0, deck1)\n"
            "  1. network play (host, port, password, deck, name)\n"
            "  2. watch replay (file)\n"
            "  3. puzzle (puzzle | deck0, deck1)\n"
            "  -1. wait\n";
        if (choice_idx == -1) return menu;
        return menu + "Error: invalid choice\n";
    }

    // Network client path. The call first drains any queued packets (opponent
    // actions, lobby updates, RPS/TP, duel prompts) with a short poll so the
    // state/choices are fresh, then applies the choice and polls for the next
    // decision. A lobby poll never blocks long: the agent re-calls to wait.
    if (net_client_connected && net_client_fd >= 0) {
        // 1. Drain queued packets (short poll) to refresh lobby / pending states.
        net_client_mode = true;
        mcp_begin_capture();
        int r = cli_read_until_prompt(150);
        std::string narration = mcp_take_capture();
        mcp_end_capture();
        net_client_mode = false;
        if (r == 2) {
            // Server rejected us (deck/side/join/version): surface + leave room.
            std::string err = cli_err_reason(cli_last_err_msg, cli_last_err_code);
            cli_net_reset();
            return "Error: " + err + " - connection closed";
        }
        if (r == -1 && net::peer_eof()) {
            cli_net_reset();
            return "Disconnected from server";
        }

        // 2. Apply the choice by current state: duel prompt, RPS, TP, lobby.
        //    -1 is always a pure poll (wait): never an error, never applied.
        std::string apply_error;
        if (cli_has_prompt && cli_stage >= 3) {
            // duel prompt: build response bytes and send CTOS_RESPONSE
            // (-1 = pass is a legitimate duel action, mirroring the GUI).
            mcp_send_choice(choice_idx, choice_indices);
            net::write_packet(net_client_fd, CTOS_RESPONSE, net_response_buf.data(), net_response_buf.size());
            cli_has_prompt = false;
        } else if (cli_pending_hand) {
            if (choice_idx != -1) {
                if (choice_idx < 1 || choice_idx > 3) {
                    apply_error = "Error: RPS choice must be 1 (rock), 2 (paper), 3 (scissors)\n";
                } else {
                    if (std::getenv("YGOCLI_MCP_DEBUG"))
                        fprintf(stderr, "[dbg] RPS apply choice=%d\n", choice_idx);
                    cli_pending_hand = 0;
                    std::vector<uint8_t> rr = {(uint8_t)choice_idx};
                    net::write_packet(net_client_fd, CTOS_HAND_RESULT, rr.data(), 1);
                }
            }
        } else if (cli_pending_tp) {
            if (choice_idx != -1) {
                if (choice_idx != 0 && choice_idx != 1) {
                    apply_error = "Error: TP choice must be 0 (first) or 1 (second)\n";
                } else {
                    cli_pending_tp = 0;
                    std::vector<uint8_t> tp = {(uint8_t)choice_idx};
                    net::write_packet(net_client_fd, CTOS_TP_RESULT, tp.data(), 1);
                }
            }
        } else if (cli_stage == 0 && choice_idx != -1) {
            // lobby action (validated against freshly-drained state)
            bool is_observer = net_client_seat >= 2;
            if (choice_idx == 0) {
                if (is_observer) net::send_packet(net_client_fd, CTOS_HS_TODUELIST);
                else if (cli_ready[net_client_seat]) net::send_packet(net_client_fd, CTOS_HS_NOTREADY);
                else {
                    std::vector<uint8_t> dk = cli_build_deck_payload(cli_deck_codes);
                    net::write_packet(net_client_fd, CTOS_UPDATE_DECK, dk.data(), dk.size());
                    net::send_packet(net_client_fd, CTOS_HS_READY);
                    cli_ready[net_client_seat] = true;
                }
            } else if (choice_idx == 1 && cli_is_host && cli_ready[0] && cli_ready[1]) {
                net::send_packet(net_client_fd, CTOS_HS_START);
            } else if (choice_idx == 2 && !is_observer && !cli_names[1 - net_client_seat].empty()) {
                net::send_packet(net_client_fd, CTOS_HS_TOOBSERVER);
            } else {
                if (std::getenv("YGOCLI_MCP_DEBUG"))
                    fprintf(stderr, "[dbg] lobby choice=%d seat=%d is_host=%d ready0=%d ready1=%d names0='%s' names1='%s'\n",
                            choice_idx, net_client_seat, (int)cli_is_host, (int)cli_ready[0], (int)cli_ready[1],
                            cli_names[0].c_str(), cli_names[1].c_str());
                apply_error = "Error: invalid lobby choice\n";
            }
        }

        // 3. Poll for the next decision: a short window in lobby (opponent may
        //    ready/start), longer once the duel is underway.
        net_client_mode = true;
        mcp_begin_capture();
        int wait_ms = (cli_stage >= 3) ? 5000 : 500;
        int r2 = cli_wait_for_prompt(wait_ms, (cli_stage >= 3) ? 120 : 4);
        std::string narration2 = mcp_take_capture();
        mcp_end_capture();
        net_client_mode = false;

        if (r2 == -1 && net::peer_eof()) {
            // Room/server went away (duel ended, host left, server stopped).
            cli_net_reset();
            std::ostringstream out;
            if (cli_match_over) out << "Match Over";
            else out << "Game Over";
            out << ": Player " << cli_winner << " wins! (disconnected)\n";
            return out.str();
        }

        std::ostringstream out;
        if (!narration2.empty()) out << narration2;
        if (!apply_error.empty()) out << apply_error;
        if (cli_stage >= 3 || cli_has_prompt) out << cli_render_field();
        if (r2 == 1) {
            if (cli_match_mode && !cli_match_over)
                out << "Duel over: Player " << cli_winner << " wins! (match continues, siding)\n";
            else if (cli_match_over)
                out << "Match Over: Player " << cli_winner << " wins the match!\n";
            else if (cli_game_over) out << "Game Over: Player " << cli_winner << " wins!\n";
            else out << "Game Over (duel ended without a winner)\n";
            return out.str();
        }
        if (cli_pending_hand) { out << "RPS choice:\n  1. rock\n  2. paper\n  3. scissors\n"; return out.str(); }
        if (cli_pending_tp)   { out << "Turn-order choice:\n  0. go first\n  1. go second\n"; return out.str(); }
        if (cli_has_prompt)   { out << mcp_build_choices(); return out.str(); }
        if (cli_stage == 0)   { out << cli_lobby_choices(); return out.str(); }
        out << "(waiting for opponent or disconnected)\n";
        return out.str();
    }

    // Solo in-process path
    if (global_pduel == 0) return "Error: No active game";

    mcp_send_choice(choice_idx, choice_indices);

    mcp_begin_capture();
    int ret = mcp_run_until_pause();
    std::string narration = mcp_take_capture();
    mcp_end_capture();

    if (ret == 1) {
        std::string result = mcp_format_output(narration, "");
        result += "Game Over: Player " + std::to_string(mcp_winner) + " wins!";
        end_duel(global_pduel);
        global_pduel = 0;
        return result;
    }
    if (ret == -1) return "Error: Game not started";

    std::string choices = mcp_build_choices();
    return mcp_format_output(narration, choices);
}

// JSON-RPC 2.0 server loop (stdin/stdout line-based)
// ============================================================
// Tool registry: single source of truth for tools/list, tools/call, and the
// interactive CLI (ygocli interact). Keep tool metadata here; handlers live
// near their dependencies (network-client tools below, solo tools above).
// ============================================================

struct ToolDef {
    const char* name;
    const char* description;
    nlohmann::json schema;      // inputSchema: {type:object, properties:{...}}
    std::string (*fn)(const nlohmann::json&);
};

// Compact input-schema builder: props = { {name, json-type, description}, ... }
static nlohmann::json tool_schema(std::initializer_list<std::array<const char*, 3>> props) {
    nlohmann::json p = nlohmann::json::object();
    for (const auto& a : props) p[a[0]] = {{"type", a[1]}, {"description", a[2]}};
    nlohmann::json s;
    s["type"] = "object";
    s["properties"] = p;
    return s;
}

// Forward declarations (handlers defined near their dependencies).
static std::string mcp_tool_ygo_choose(const nlohmann::json& params);
static std::string mcp_tool_ygo_single_mode(const nlohmann::json& params);
static std::string mcp_tool_card_search(const nlohmann::json& params);
static std::string mcp_tool_ygo_chat(const nlohmann::json& params);
static std::string mcp_tool_ygo_disconnect(const nlohmann::json& params);
static std::string mcp_tool_ygo_surrender(const nlohmann::json& params);
static std::string mcp_tool_ygo_replay(const nlohmann::json& params);
static std::string mcp_tool_ygo_puzzle(const nlohmann::json& params);
static std::string mcp_tool_ygo_wiki(const nlohmann::json& params);

static const std::vector<ToolDef>& tools_registry() {
    static const std::vector<ToolDef> tools = {
        {"ygo_choose",
         "Choose an option from the pending decision and continue. With no active game "
         "this is the main menu (solo / network / replay / puzzle). In a room lobby it "
         "offers ready/observer/start/wait; then RPS and turn-order choices; in a duel it "
         "offers the prompt options (or -1 to pass). Choices with only one real option are "
         "answered automatically and never pause.",
         tool_schema({{"id", "integer", "The choice index to select (or -1 to pass/wait)"},
                      {"indices", "array", "For multi-select prompts, ordered list of indices"},
                      {"host", "string", "network play: server host (default 127.0.0.1)"},
                      {"port", "integer", "network play: server port (default 7911)"},
                      {"password", "string", "network play: room key (empty = random match)"},
                      {"deck", "array", "network play: deck (.ydk path or card-code array)"},
                      {"deck0", "string", "solo/puzzle: deck0 path"},
                      {"deck1", "string", "solo/puzzle: deck1 path"},
                      {"name", "string", "network play: player name (default ygocli)"},
                      {"file", "string", "replay: path to .yrp file"},
                      {"puzzle", "string", "puzzle: name -> single/<puzzle>/ decks"},
                      {"lp", "integer", "Starting LP (default 8000)"},
                      {"start_hand", "integer", "Starting hand size (default 5)"},
                      {"draw_count", "integer", "Cards drawn per turn (default 1)"},
                      {"rule", "integer", "Master rule version"}}),
         mcp_tool_ygo_choose},
        {"ygo_chat",
         "Send a chat message to the current room (players and observers).",
         tool_schema({{"text", "string", "Message text"}}),
         mcp_tool_ygo_chat},
        {"ygo_card",
         "Search the card database by any combination of filters (AND). Usable in-game or out.",
         tool_schema({{"id", "integer", "Exact card code"},
                      {"text", "string", "Words that must appear in name/desc"},
                      {"level", "string", "Level filter, e.g. 4, >4, <=8"},
                      {"atk", "string", "ATK filter, e.g. >2500"},
                      {"def", "string", "DEF filter"},
                      {"type", "string", "Type filter, e.g. Monster|Effect|Xyz"},
                      {"race", "string", "Race filter, e.g. Dragon|Machine"},
                      {"attribute", "string", "Attribute filter, e.g. Fire|Light"}}),
         mcp_tool_card_search},
        {"ygo_wiki",
         "Search the bundled wiki (wiki/ folder of small markdown files) for concepts. "
         "Returns the content of matching files. Usable in-game or out.",
         tool_schema({{"text", "string", "Concept to search for (empty lists all topics)"}}),
         mcp_tool_ygo_wiki},
        {"ygo_surrender",
         "Surrender the current game. The connection stays up: in a match the opponent "
         "wins the game, players exchange side decks, and the next game starts.",
         tool_schema({}),
         mcp_tool_ygo_surrender},
        {"ygo_disconnect",
         "Leave the current room / close the network connection (or exit when nothing is "
         "connected).",
         tool_schema({}),
         mcp_tool_ygo_disconnect},
    };
    return tools;
}

// ---------------------------------------------------------------------------
// FUTURE (not implemented, do NOT register): ygo_snapshot
//   Snapshot the current game state into a ygo_puzzle format (single/ folder):
//   export decks, LP, hand/field/grave/removed contents per player, turn/phase,
//   and the win condition, as a puzzle that ygo_puzzle can reload.
//   Hard parts: expressing arbitrary state (counters, markers, chain state,
//   xyz materials, pending effects) in a script-based puzzle format. The puzzle
//   loader (mcp_init_duel_setup) already runs a setup.lua in the duel's lua
//   context (Duel.SetLP etc.), so a snapshot can be emitted as a setup.lua +
//   two decks. Where the ocgcore lua API lacks the needed helpers (e.g. placing
//   a specific card in a zone with materials), the plan is to fork ocgcore and
//   add new Duel.* functions so arbitrary states are expressible.
//   Do not expose in tools/list until implemented.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ygo_wiki: grep-based search over wiki/*.md concept files.
// ---------------------------------------------------------------------------
static std::string mcp_tool_ygo_wiki(const nlohmann::json& params) {
    std::string text = params.value("text", "");
    std::vector<std::string> files;
    glob_t g;
    std::string pat = g_exe_dir + "/wiki/*.md";
    if (::glob(pat.c_str(), 0, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; ++i) files.push_back(g.gl_pathv[i]);
        globfree(&g);
    }
    if (files.empty()) return "wiki/ folder is empty or missing (looked in " + pat + ")";
    std::sort(files.begin(), files.end());

    if (text.empty()) {
        std::ostringstream out;
        out << "wiki/ topics (" << files.size() << "):\n";
        for (auto& f : files) {
            size_t s = f.find_last_of('/');
            out << "  " << f.substr(s + 1) << "\n";
        }
        out << "\nCall ygo_wiki with a text to see matching files.";
        return out.str();
    }

    std::string lower;
    for (char c : text) lower += (char)std::tolower((unsigned char)c);

    std::ostringstream out;
    int shown = 0;
    for (auto& f : files) {
        std::ifstream in(f);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string content = ss.str();
        std::string cl = content;
        for (auto& c : cl) c = (char)std::tolower((unsigned char)c);
        if (cl.find(lower) == std::string::npos) continue;
        size_t s = f.find_last_of('/');
        out << "===== " << f.substr(s + 1) << " =====\n" << content << "\n\n";
        if (++shown >= 3) { out << "(more matches omitted; refine your search)\n"; break; }
    }
    if (shown == 0) return "No wiki file mentions '" + text + "'. Try ygo_wiki with no text to list topics.";
    return out.str();
}

static std::string mcp_tool_ygo_puzzle(const nlohmann::json& params) {
    std::string deck0 = params.value("deck0", "");
    std::string deck1 = params.value("deck1", "");
    std::string puzzle = params.value("puzzle", "");
    std::string setup_lua;
    if (!puzzle.empty()) {
        std::string base = g_exe_dir + "/single/" + puzzle;
        if (deck0.empty()) deck0 = base + "/deck0.ydk";
        if (deck1.empty()) deck1 = base + "/deck1.ydk";
        setup_lua = base + "/setup.lua";
        if (::access(setup_lua.c_str(), R_OK) != 0) setup_lua.clear();
    }
    int lp = params.value("lp", 8000);
    int start_hand = params.value("start_hand", 5);
    int draw_count = params.value("draw_count", 1);
    int rule = params.value("rule", CURRENT_RULE);

    if (!mcp_init_duel(deck0, deck1, lp, start_hand, draw_count, rule, setup_lua)) {
        return "Error: failed to load puzzle (deck0=" + deck0 + " deck1=" + deck1 + ")";
    }
    mcp_begin_capture();
    int ret = mcp_run_until_pause();
    std::string narration = mcp_take_capture();
    mcp_end_capture();
    if (ret == 1) {
        std::string result = mcp_format_output(narration, "");
        result += "Game Over: Player " + std::to_string(mcp_winner) + " wins!";
        end_duel(global_pduel);
        global_pduel = 0;
        return result;
    }
    if (ret == -1) return "Error: Game not started";
    std::string choices = mcp_build_choices();
    return mcp_format_output(narration, choices);
}

// ---------------------------------------------------------------------------
// ygocli interact: same engine as MCP, but a human picks the tool and fills
// each field interactively. Output format is identical to tools/call.
// ---------------------------------------------------------------------------
static void mcp_interact_loop() {
    auto print_help = []() {
        std::cout << "Available tools:\n";
        for (const auto& t : tools_registry()) std::cout << "  " << t.name << "\n";
        std::cout << "Type a tool name, 'help', or 'quit'.\n";
    };
    std::cout << "ygocli interactive mode (same engine as MCP, human-friendly input)\n";
    print_help();
    std::string line;
    while (true) {
        std::cout << "\nygocli> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        if (line == "quit" || line == "exit" || line == "q") break;
        if (line == "help" || line == "?") { print_help(); continue; }
        const ToolDef* tool = nullptr;
        for (const auto& t : tools_registry()) if (line == t.name) { tool = &t; break; }
        if (!tool) { std::cout << "Unknown tool '" << line << "'. Type 'help'.\n"; continue; }

        nlohmann::json args = nlohmann::json::object();
        auto props = tool->schema.value("properties", nlohmann::json::object());
        for (auto it = props.begin(); it != props.end(); ++it) {
            std::string type = it.value().value("type", "string");
            std::cout << "  " << it.key() << " (" << type << ") - "
                      << it.value().value("description", "") << " [empty=skip]: " << std::flush;
            std::string val;
            if (!std::getline(std::cin, val)) break;
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.pop_back();
            if (val.empty()) continue;
            if (type == "integer") {
                args[it.key()] = (nlohmann::json::number_integer_t)std::strtoll(val.c_str(), nullptr, 10);
            } else if (type == "boolean") {
                args[it.key()] = (val == "true" || val == "1" || val == "yes" || val == "y");
            } else if (type == "array") {
                nlohmann::json arr = nlohmann::json::array();
                std::string tok;
                std::stringstream ss(val);
                while (std::getline(ss, tok, ',')) {
                    if (!tok.empty()) arr.push_back(tok);
                }
                args[it.key()] = arr;
            } else {
                args[it.key()] = val;
            }
        }
        std::cout << "----------------------------\n";
        // Interactive mode: keep blocking while the tool reports it is merely
        // waiting (host waiting for the opponent to join, polling for the next
        // prompt, observing) instead of bouncing back to the ygocli> prompt.
        // These tools are safe to re-invoke: with no pending prompt they only
        // poll the socket and never resend anything.
        std::string result = tool->fn(args);
        while (result.find("(waiting for opponent") != std::string::npos
               || result.find("(watching") != std::string::npos
               || result.find("(observing") != std::string::npos) {
            result = tool->fn(args);
        }
        std::cout << result << "\n";
        std::cout.flush();
    }
}

// JSON-RPC 2.0 server loop (stdin/stdout line-based), MCP-compatible.
static void mcp_jsonrpc_loop() {
    std::string line;
    while (true) {
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        nlohmann::json req;
        try {
            req = nlohmann::json::parse(line);
        } catch (const std::exception&) {
            nlohmann::json err = {{"jsonrpc", "2.0"}, {"id", nullptr},
                                  {"error", {{"code", -32700}, {"message", "Parse error"}}}};
            std::cout << err.dump() << "\n";
            std::cout.flush();
            continue;
        }

        std::string method = req.value("method", "");
        bool is_notification = !req.contains("id") || req["id"].is_null();
        bool needs_response = !is_notification;

        nlohmann::json resp;
        resp["jsonrpc"] = "2.0";
        if (!is_notification) resp["id"] = req["id"];

        int error_code = 0;
        std::string error_msg;

        if (method == "initialize") {
            nlohmann::json init_params = req.value("params", nlohmann::json::object());
            std::string pv = init_params.value("protocolVersion", "2024-11-05");
            resp["result"] = {
                {"protocolVersion", pv},
                {"serverInfo", {{"name", "ygocli"}, {"version", "1.0"}}},
                {"capabilities", {{"tools", nlohmann::json::object()}}}
            };
        } else if (method == "tools/list") {
            nlohmann::json tools = nlohmann::json::array();
            for (const auto& t : tools_registry()) {
                tools.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.schema}});
            }
            resp["result"]["tools"] = tools;
        } else if (method == "tools/call") {
            nlohmann::json params = req.value("params", nlohmann::json::object());
            std::string tool_name = params.value("name", "");
            nlohmann::json args = params.value("arguments", nlohmann::json::object());
            if (!args.is_object()) args = nlohmann::json::object();

            std::string tool_result;
            const ToolDef* tool = nullptr;
            for (const auto& t : tools_registry()) if (tool_name == t.name) { tool = &t; break; }
            if (!tool) {
                error_code = -32601;
                error_msg = "Unknown tool: " + tool_name;
            } else {
                tool_result = tool->fn(args);
            }
            if (error_code == 0) {
                resp["result"] = {{"content", {{{"type", "text"}, {"text", tool_result}}}}};
            }
        } else if (method == "notifications/initialized") {
            needs_response = false;
        } else {
            error_code = -32601;
            error_msg = "Unknown method: " + method;
        }

        if (error_code != 0) {
            resp["error"] = {{"code", error_code}, {"message", error_msg}};
        }
        if (needs_response) {
            std::cout << resp.dump() << "\n";
            std::cout.flush();
        }
    }
}


static void cli_clear_field() { cli_field.clear(); cli_has_prompt = false; cli_game_over = false; }

// Parse one query record (from MSG_UPDATE_DATA payload) into a CliCard.
static void cli_parse_record(uint8_t*& p, uint8_t* end, CliCard& out) {
    out = CliCard();
    if (p + 8 > end) return;
    int32_t clen = BufferIO::Read<int32_t>(p);
    if (clen < 8 || p + (clen - 4) > end) { p = end; return; }
    uint8_t* rec_end = p + (clen - 4);
    int32_t flag = BufferIO::Read<int32_t>(p);
    if (flag & QUERY_CODE) { out.code = BufferIO::Read<uint32_t>(p); }
    if (flag & QUERY_POSITION) { uint32_t il = BufferIO::Read<uint32_t>(p); out.pos = (il >> 24) & 0xff; }
    if (flag & QUERY_ALIAS) { BufferIO::Read<uint32_t>(p); }
    if (flag & QUERY_TYPE) { BufferIO::Read<uint32_t>(p); }
    if (flag & QUERY_LEVEL) { out.level = BufferIO::Read<uint32_t>(p); }
    if (flag & QUERY_RANK) { out.rank = BufferIO::Read<uint32_t>(p); }
    if (flag & QUERY_ATTRIBUTE) { BufferIO::Read<uint32_t>(p); }
    if (flag & QUERY_RACE) { BufferIO::Read<uint32_t>(p); }
    if (flag & QUERY_ATTACK) { out.atk = BufferIO::Read<int32_t>(p); }
    if (flag & QUERY_DEFENSE) { out.def = BufferIO::Read<int32_t>(p); }
    out.present = (out.code != 0);
    p = rec_end;
}

// Handle a single MSG_* from the STOC_GAME_MSG stream, updating the field model
// and (for prompts) storing the prompt. Returns 0=continue, 1=prompt stored,
// 2=game over.
static int cli_handle_msg(uint8_t*& p, uint8_t* end) {
    uint8_t msg = BufferIO::Read<uint8_t>(p);
    switch (msg) {
    case MSG_UPDATE_DATA: {
        int player = BufferIO::Read<uint8_t>(p);
        int loc = BufferIO::Read<uint8_t>(p);
        auto& vec = cli_field[{player, loc}];
        vec.clear();
        while (p < end) {
            uint8_t* rec = p;
            if (p + 4 > end) break;
            int32_t clen = BufferIO::Read<int32_t>(p);
            if (clen <= 4) continue;  // empty slot (clen==4): skip, keep stream in sync
            p = rec; // rewind; parse_record reads clen itself
            CliCard c;
            cli_parse_record(p, end, c);
            if (!c.present && vec.empty()) continue; // empty slot
            vec.push_back(c);
        }
        // Note: empty slots are skipped; seq used for rendering only.
        break;
    }
    case MSG_UPDATE_CARD: {
        int player = BufferIO::Read<uint8_t>(p);
        int loc = BufferIO::Read<uint8_t>(p);
        int seq = BufferIO::Read<uint8_t>(p);
        CliCard c;
        cli_parse_record(p, end, c);
        auto& vec = cli_field[{player, loc}];
        if ((int)vec.size() <= seq) vec.resize(seq + 1);
        vec[seq] = c;
        break;
    }
    case MSG_MOVE: {
        uint32_t code = BufferIO::Read<uint32_t>(p);
        uint32_t prev = BufferIO::Read<uint32_t>(p);
        uint32_t nw = BufferIO::Read<uint32_t>(p);
        BufferIO::Read<uint32_t>(p); // reason
        int pc = prev & 0xff, pl = (prev >> 8) & 0xff, ps = (prev >> 16) & 0xff;
        int nc = nw & 0xff, nl = (nw >> 8) & 0xff, ns = (nw >> 16) & 0xff;
        uint8_t pos = (nw >> 24) & 0xff;
        // remove from old location
        auto it = cli_field.find({pc, pl});
        if (it != cli_field.end() && (int)it->second.size() > ps) it->second[ps] = CliCard();
        // insert into new location
        auto& v = cli_field[{nc, nl}];
        if ((int)v.size() <= ns) v.resize(ns + 1);
        CliCard c; c.code = code; c.pos = pos; c.present = true;
        v[ns] = c;
        break;
    }
    case MSG_NEW_TURN: { BufferIO::Read<uint8_t>(p); cli_turn++; break; }
    case MSG_NEW_PHASE: { cli_phase = BufferIO::Read<uint16_t>(p); break; }
    case MSG_LPUPDATE: {
        int pl = BufferIO::Read<uint8_t>(p);
        cli_lp[pl] = BufferIO::Read<int32_t>(p);
        break;
    }
    case MSG_DAMAGE: {
        int pl = BufferIO::Read<uint8_t>(p);
        cli_lp[pl] -= BufferIO::Read<int32_t>(p);
        break;
    }
    case MSG_RECOVER: {
        int pl = BufferIO::Read<uint8_t>(p);
        cli_lp[pl] += BufferIO::Read<int32_t>(p);
        break;
    }
    case MSG_WIN: {
        cli_winner = BufferIO::Read<uint8_t>(p);
        BufferIO::Read<uint8_t>(p);
        cli_game_over = true;
        return 2;
    }
    case MSG_RETRY: {
        // engine rejected the previous response; re-surface the stored prompt
        cli_has_prompt = true;
        return 1;
    }
    default: {
        // Prompt messages: store raw payload for choice building.
        // The stored prompt is the REMAINING bytes of this STOC_GAME_MSG packet
        // (the server sends a prompt as the last message in its packet).
        bool is_prompt = false;
        switch (msg) {
        case MSG_SELECT_BATTLECMD: case MSG_SELECT_IDLECMD: case MSG_SELECT_EFFECTYN:
        case MSG_SELECT_YESNO: case MSG_SELECT_OPTION: case MSG_SELECT_CARD:
        case MSG_SELECT_CHAIN: case MSG_SELECT_PLACE: case MSG_SELECT_POSITION:
        case MSG_SELECT_TRIBUTE: case MSG_SELECT_COUNTER: case MSG_SELECT_SUM:
        case MSG_SELECT_DISFIELD: case MSG_SORT_CARD: case MSG_SELECT_UNSELECT_CARD:
        case MSG_ROCK_PAPER_SCISSORS: case MSG_ANNOUNCE_RACE: case MSG_ANNOUNCE_ATTRIB:
        case MSG_ANNOUNCE_CARD: case MSG_ANNOUNCE_NUMBER:
            is_prompt = true;
            break;
        default: break;
        }
        if (is_prompt) {
            uint8_t player = (p < end) ? *p : 0;
            mcp_store_prompt_raw(msg, player, p, (size_t)(end - p));
            // Prompts with no real decision (only pass / a single forced option)
            // are answered right here: build the response bytes, send
            // CTOS_RESPONSE, and keep reading so the agent is not woken until a
            // genuine choice or the game ends.
            if (net_client_mode && net_client_connected && net_client_fd >= 0) {
                int auto_idx = -1;
                if (mcp_prompt_is_forced(auto_idx)) {
                    mcp_send_choice(auto_idx, {});
                    net::write_packet(net_client_fd, CTOS_RESPONSE, net_response_buf.data(), net_response_buf.size());
                    cli_has_prompt = false;
                    std::cout << "[auto] " << msg_type_name(msg)
                              << ": no real choice, responding automatically\n";
                    if (std::getenv("YGOCLI_MCP_DEBUG"))
                        fprintf(stderr, "cli auto-responded to forced prompt %d (idx %d)\n", (int)msg, auto_idx);
                    p = end; // prompt payload consumed; nothing else follows
                    return 0;
                }
            }
            cli_has_prompt = true;
            return 1;
        }
        // Informational: advance past the exact payload so the stream stays
        // in sync (mirrors the server's srv_analyze skip lengths).
        switch (msg) {
        case MSG_START: {
            // A new game begins (also each game of a match): reset game state.
            cli_field.clear();
            cli_has_prompt = false;
            cli_game_over = false;
            cli_winner = -1;
            cli_turn = 1;
            cli_phase = 0x04;
            cli_stage = 3;   // dueling
            // gframe MSG_START: player(1) rule(1) lp0(4) lp1(4) deck0(2) extra0(2) deck1(2) extra1(2)
            BufferIO::Read<uint8_t>(p); // player
            BufferIO::Read<uint8_t>(p); // rule
            cli_lp[0] = BufferIO::Read<int32_t>(p);
            cli_lp[1] = BufferIO::Read<int32_t>(p);
            uint16_t d0 = BufferIO::Read<uint16_t>(p);
            uint16_t e0 = BufferIO::Read<uint16_t>(p);
            uint16_t d1 = BufferIO::Read<uint16_t>(p);
            BufferIO::Read<uint16_t>(p); // extra1
            cli_field[{0, LOCATION_DECK}].assign(d0, CliCard{});
            cli_field[{1, LOCATION_DECK}].assign(d1, CliCard{});
            cli_field[{0, LOCATION_EXTRA}].assign(e0, CliCard{});
            if (std::getenv("YGOCLI_MCP_DEBUG"))
                fprintf(stderr, "cli MSG_START parsed lp0=%d lp1=%d d0=%d e0=%d d1=%d\n", cli_lp[0], cli_lp[1], d0, e0, d1);
            break;
        }
        case MSG_HINT: { uint8_t t=BufferIO::Read<uint8_t>(p); uint8_t pl=BufferIO::Read<uint8_t>(p); BufferIO::Read<uint32_t>(p); (void)t;(void)pl; break; }
        case MSG_CONFIRM_DECKTOP:
        case MSG_CONFIRM_EXTRATOP: { BufferIO::Read<uint8_t>(p); uint8_t c=BufferIO::Read<uint8_t>(p); p += c*7; break; }
        case MSG_CONFIRM_CARDS: { BufferIO::Read<uint8_t>(p); BufferIO::Read<uint8_t>(p); uint8_t c=BufferIO::Read<uint8_t>(p); p += c*7; break; }
        case MSG_SHUFFLE_DECK: BufferIO::Read<uint8_t>(p); break;
        case MSG_SHUFFLE_HAND:
        case MSG_SHUFFLE_EXTRA: { BufferIO::Read<uint8_t>(p); uint8_t c=BufferIO::Read<uint8_t>(p); p += c*4; break; }
        case MSG_SWAP_GRAVE_DECK: BufferIO::Read<uint8_t>(p); break;
        case MSG_REVERSE_DECK: break;
        case MSG_DECK_TOP: p += 6; break;
        case MSG_SHUFFLE_SET_CARD: { BufferIO::Read<uint8_t>(p); uint8_t c=BufferIO::Read<uint8_t>(p); p += c*8; break; }
        case MSG_POS_CHANGE: p += 9; break;
        case MSG_SET: p += 8; break;   // code(4) + info_location(4)
        case MSG_SWAP: p += 16; break;
        case MSG_FIELD_DISABLED: p += 4; break;
        case MSG_SUMMONING:
        case MSG_SPSUMMONING:
        case MSG_FLIPSUMMONING: p += 8; break;
        case MSG_SUMMONED: case MSG_SPSUMMONED: case MSG_FLIPSUMMONED: break;
        case MSG_CHAINING: p += 16; break;
        case MSG_CHAINED: case MSG_CHAIN_SOLVING: case MSG_CHAIN_SOLVED:
        case MSG_CHAIN_NEGATED: case MSG_CHAIN_DISABLED: p += 1; break;
        case MSG_CHAIN_END: break;
        case MSG_CARD_SELECTED: { BufferIO::Read<uint8_t>(p); uint8_t c=BufferIO::Read<uint8_t>(p); p += c*4; break; }
        case MSG_RANDOM_SELECTED: { BufferIO::Read<uint8_t>(p); uint8_t c=BufferIO::Read<uint8_t>(p); p += c*4; break; }
        case MSG_BECOME_TARGET: { uint8_t c=BufferIO::Read<uint8_t>(p); p += c*4; break; }
        case MSG_DRAW: {
            int pl = BufferIO::Read<uint8_t>(p);
            uint8_t c = BufferIO::Read<uint8_t>(p);
            p += c*4;
            auto& dv = cli_field[{pl, LOCATION_DECK}];
            if ((int)dv.size() >= c) dv.resize(dv.size() - c); else dv.clear();
            break;
        }
        case MSG_DAMAGE: case MSG_RECOVER: case MSG_LPUPDATE:
        case MSG_PAY_LPCOST: p += 5; break;
        case MSG_EQUIP: p += 8; break;
        case MSG_UNEQUIP: p += 4; break;
        case MSG_CARD_TARGET: case MSG_CANCEL_TARGET: p += 8; break;
        case MSG_ADD_COUNTER: case MSG_REMOVE_COUNTER: p += 7; break;
        case MSG_ATTACK: p += 8; break;
        case MSG_BATTLE: p += 26; break;
        case MSG_ATTACK_DISABLED: case MSG_DAMAGE_STEP_START: case MSG_DAMAGE_STEP_END: break;
        case MSG_MISSED_EFFECT: p += 8; break;
        case MSG_TOSS_COIN: case MSG_TOSS_DICE: { BufferIO::Read<uint8_t>(p); uint8_t c=BufferIO::Read<uint8_t>(p); p += c; break; }
        case MSG_HAND_RES: p += 1; break;
        case MSG_CARD_HINT: p += 9; break;
        case MSG_PLAYER_HINT: p += 6; break;
        case MSG_UPDATE_DATA: case MSG_UPDATE_CARD: case MSG_MOVE: break; // handled above
        default: break; // unknown: cannot skip reliably
        }
        break;
    }
    }
    return 0;
}

// Process one STOC_GAME_MSG packet payload (a concatenated MSG_* stream).
static int cli_process_game_msg(uint8_t* data, size_t len) {
    uint8_t* p = data;
    uint8_t* end = data + len;
    int last = 0;
    while (p < end) {
        last = cli_handle_msg(p, end);
        if (last != 0) break;
    }
    return last;
}

// Receive STOC packets until we have a prompt or game over or timeout.
// Handles pre-duel handshake: STOC_SELECT_HAND (RPS) and STOC_SELECT_TP.
// Returns 0 if a prompt is pending (cli_has_prompt), 1 game over, -1 waiting/timeout.
// Returns: 0 = prompt pending (cli_has_prompt), 1 = game over (MSG_WIN or
// STOC_DUEL_END), 2 = fatal error received from server (STOC_ERROR_MSG),
// -1 = timeout / disconnected.
// Build a CTOS_UPDATE_DECK payload (mainc+sidec header + card codes). Used for
// the initial upload and for the match side-deck re-upload.
static std::vector<uint8_t> cli_build_deck_payload(const std::vector<uint32_t>& codes) {
    std::vector<uint8_t> dk;
    uint32_t mainc = (uint32_t)codes.size();
    uint32_t sidec = 0;
    uint8_t hdr[8];
    std::memcpy(hdr, &mainc, 4);
    std::memcpy(hdr + 4, &sidec, 4);
    dk.insert(dk.end(), hdr, hdr + 8);
    for (uint32_t c : codes) {
        uint8_t b[4];
        std::memcpy(b, &c, 4);
        dk.insert(dk.end(), b, b + 4);
    }
    return dk;
}

static int cli_read_until_prompt(int timeout_ms) {
    std::vector<uint8_t> buf;
    while (true) {
        int n = net::read_packet(net_client_fd, buf, timeout_ms);
        if (n < 0) return -1;               // timeout / disconnected
        if (buf.empty()) return -1;
        uint8_t proto = buf[0];
        const uint8_t* payload = buf.data() + 1;
        size_t plen = buf.size() - 1;
        if (proto == STOC_GAME_MSG && plen > 0) {
            if (std::getenv("YGOCLI_MCP_DEBUG")) {
                fprintf(stderr, "cli GAME_MSG len%zu:", plen);
                for (size_t i = 0; i < plen && i < 40; ++i) fprintf(stderr, " %02x", (unsigned)payload[i]);
                fprintf(stderr, "\n");
            }
            int r = cli_process_game_msg(const_cast<uint8_t*>(payload), plen);
            if (r == 1) return 0;           // prompt pending
            if (r == 2) return 1;           // game over
            continue;
        }
        if (proto == STOC_ERROR_MSG) {
            // Server rejected us (room full / deck rejected / version / game started).
            if (plen >= 8) {
                std::memcpy(&cli_last_err_msg, payload, 4);
                std::memcpy(&cli_last_err_code, payload + 4, 4);
            }
            return 2;
        }
        if (proto == STOC_SELECT_HAND) {
            // RPS: expose to the agent (1=rock, 2=paper, 3=scissors).
            cli_stage = 1;
            cli_pending_hand = 1;
            return 0;
        }
        if (proto == STOC_SELECT_TP) {
            // TP: expose to the agent (0=go first, 1=go second).
            cli_stage = 2;
            cli_pending_tp = 1;
            return 0;
        }
        if (proto == STOC_JOIN_GAME && plen >= 6) {
            // HostInfo in payload: lflist(4) rule(1) mode(1) ...
            cli_match_mode = (payload[5] == MODE_MATCH);
            cli_match_over = false;
            continue;
        }
        if (proto == STOC_CHANGE_SIDE) {
            // Match side-deck phase: re-upload our deck (side changes are not yet
            // exposed; same-deck continuation keeps the match flowing).
            if (!cli_deck_codes.empty()) {
                std::vector<uint8_t> dk = cli_build_deck_payload(cli_deck_codes);
                net::write_packet(net_client_fd, CTOS_UPDATE_DECK, dk.data(), dk.size());
            }
            continue;
        }
        if (proto == STOC_DUEL_END) {
            if (cli_match_mode) cli_match_over = true;
            return 1;
        }
        if (proto == STOC_HS_PLAYER_ENTER && plen >= 41) {
            int pos = payload[40];
            if (pos >= 0 && pos < 2) {
                std::string pn;
                for (int i = 0; i < 20; ++i) {
                    uint16_t c = payload[i * 2] | (payload[i * 2 + 1] << 8);
                    if (!c) break;
                    pn += (char)c;
                }
                cli_names[pos] = pn;
            }
            continue;
        }
        if (proto == STOC_HS_PLAYER_CHANGE && plen >= 1) {
            if (std::getenv("YGOCLI_MCP_DEBUG"))
                fprintf(stderr, "[dbg] HS_CHANGE status=0x%02x\n", (unsigned)payload[0]);
            int pos = payload[0] >> 4;
            int st = payload[0] & 0x0f;
            if (pos < 2) {
                if (st == PLAYERCHANGE_LEAVE) cli_names[pos].clear();
                cli_ready[pos] = (st == PLAYERCHANGE_READY);
            }
            continue;
        }
        // other STOC (join/type/hs) — ignore for play
    }
}

// Block until a prompt is pending for us, the game ends, or the opponent goes
// silent for max_idle consecutive timeout windows. Returns like
// cli_read_until_prompt: 0 = prompt pending (cli_has_prompt), 1 = game over,
// -1 = idle too long (timeout/disconnect). Packets streaming in while the
// opponent plays reset the idle counter, so this only trips on true silence.
static int cli_wait_for_prompt(int timeout_ms, int max_idle) {
    int idle = 0;
    while (true) {
        int r = cli_read_until_prompt(timeout_ms);
        if (r != -1) return r;
        if (++idle >= max_idle) return -1;
    }
}

// Render the field from the model (per player, from our seat's perspective).
static std::string cli_render_field() {
    std::ostringstream out;
    out << "=== Turn " << cli_turn << " Phase " << phase_name(cli_phase) << " ===\n";
    for (int pl = 0; pl < 2; ++pl) {
        out << "P" << pl << " LP:" << cli_lp[pl];
        out << " Deck:" << (int)(cli_field[{pl, LOCATION_DECK}].size())
            << " Extra:" << (int)(cli_field[{pl, LOCATION_EXTRA}].size()) << "\n";
        auto loc_str = [&](int loc, const char* label, bool names) {
            auto it = cli_field.find({pl, loc});
            std::vector<CliCard> vec = (it == cli_field.end()) ? std::vector<CliCard>() : it->second;
            bool onfield = (loc == LOCATION_MZONE || loc == LOCATION_SZONE);
            bool any = false;
            std::ostringstream s;
            for (size_t i = 0; i < vec.size(); ++i) {
                if (!vec[i].present) continue;
                if (any) s << ", ";
                any = true;
                if (onfield) s << "[" << i << "] ";
                if (names && vec[i].code) {
                    s << get_card_name(vec[i].code);
                    if (loc == LOCATION_MZONE) {
                        s << " " << pos_name(vec[i].pos);
                        s << mcp_lv_label(vec[i].code, vec[i].level, vec[i].rank);
                        s << " " << vec[i].atk << "/" << vec[i].def;
                    } else if (loc == LOCATION_SZONE) {
                        s << " " << st_pos_label(vec[i].pos);
                    }
                } else if (onfield) {
                    s << "set";
                } else {
                    s << "?";
                }
            }
            out << "  " << label << ": " << (any ? s.str() : "-") << "\n";
        };
        loc_str(LOCATION_HAND, "Hand", true);
        loc_str(LOCATION_MZONE, "MZone", true);
        loc_str(LOCATION_SZONE, "SZone", true);
        loc_str(LOCATION_GRAVE, "Grave", true);
        loc_str(LOCATION_REMOVED, "Removed", true);
    }
    return out.str();
}

// Resolve the ygo_client/ygo_observe "deck" param: a path to a .ydk file, or an
// array of card codes. Returns the code list (main+extra unsplit) or false.
static bool resolve_deck_codes(const nlohmann::json& deck, std::vector<uint32_t>& codes) {
    codes.clear();
    if (deck.is_string()) {
        Deck d = load_deck(deck.get<std::string>());
        if (d.main.empty() && d.extra.empty()) return false;
        codes.insert(codes.end(), d.main.begin(), d.main.end());
        codes.insert(codes.end(), d.extra.begin(), d.extra.end());
        return true;
    }
    if (deck.is_array()) {
        for (const auto& v : deck) {
            if (v.is_number()) codes.push_back((uint32_t)v.get<int64_t>());
            else if (v.is_string()) codes.push_back((uint32_t)std::strtoul(v.get<std::string>().c_str(), nullptr, 10));
        }
        return !codes.empty();
    }
    return false;
}

// UTF-16LE writer for gframe name/pass fields (narrow chars -> low bytes).
static void utf16_write(std::vector<uint8_t>& v, size_t offset, const std::string& s, size_t max_chars) {
    for (size_t i = 0; i < s.size() && i < max_chars; ++i) v[offset + i * 2] = (uint8_t)s[i];
}

// Connect + handshake to the server. Returns 0 on success, -1 on I/O failure,
// or the STOC_ErrorMsg code (ERRMSG_*) when the server rejects us.
// Connect + handshake to a YGOPro server (e.g. srvpro). The room is
// join-or-create by password: CTOS_JOIN_GAME with pass = room key; servers
// create the room when it does not exist (srvpro: ROOM_find_or_create_by_name).
// Empty pass => server-side random matching (srvpro: S/M/T types). Host info
// (LP/rule/mode) comes from the server config, not the client.
// Returns 0 on success, -1 on I/O failure, or the STOC_ErrorMsg code.
static int cli_connect_and_join(const std::string& host, uint16_t port,
                                const std::vector<uint32_t>& deck_codes,
                                const std::string& name, const std::string& password) {
    net_client_fd = net::connect(host, port);
    if (net_client_fd < 0) return -1;
    net_client_connected = true;
    cli_deck_codes = deck_codes;
    cli_is_host = false;
    cli_stage = 0;            // lobby
    cli_ready[0] = cli_ready[1] = false;
    cli_names[0] = cli_names[1] = "";
    cli_pending_hand = 0;
    cli_pending_tp = 0;
    cli_match_mode = false;
    cli_match_over = false;

    // CTOS_PLAYER_INFO
    std::vector<uint8_t> ni(40, 0);
    utf16_write(ni, 0, name, 20);
    net::write_packet(net_client_fd, CTOS_PLAYER_INFO, ni.data(), 40);

    // CTOS_JOIN_GAME: version + gameid(0) + pass (room key)
    std::vector<uint8_t> jg(sizeof(CTOS_JoinGame), 0);
    uint16_t ver = PRO_VERSION;
    std::memcpy(jg.data(), &ver, 2);
    uint32_t gameid = 0;
    std::memcpy(jg.data() + 4, &gameid, 4);
    utf16_write(jg, 8, password, 20);
    net::write_packet(net_client_fd, CTOS_JOIN_GAME, jg.data(), jg.size());

    // read STOC_JOIN_GAME + STOC_TYPE_CHANGE to learn our seat/role;
    // STOC_ERROR_MSG (room full / version / game started) aborts with its code.
    std::vector<uint8_t> buf;
    bool got_type = false;
    while (!got_type) {
        int n = net::read_packet(net_client_fd, buf, 3000);
        if (n < 0) return -1;
        if (buf.empty()) return -1;
        if (buf[0] == STOC_ERROR_MSG && n >= 8) {
            std::memcpy(&cli_last_err_msg, buf.data() + 1, 4);
            std::memcpy(&cli_last_err_code, buf.data() + 5, 4);
            return -2;   // connected, but the server rejected the join
        }
        if (buf[0] == STOC_TYPE_CHANGE && n >= 1) {
            // high bit 0x10 = host (first player in room), low nibble = role
            cli_is_host = (buf[1] & 0x10) != 0;
            net_client_seat = buf[1] & 0x0f;
            got_type = true;
        }
        if (buf[0] == STOC_JOIN_GAME && n >= 6) {
            cli_match_mode = (buf[6] == MODE_MATCH);   // HostInfo.mode at payload offset 5
        }
        if (buf[0] == STOC_HS_PLAYER_ENTER && n >= 41) {
            int pos = buf[41];
            if (pos >= 0 && pos < 2) {
                std::string pn;
                for (int i = 0; i < 20; ++i) { uint16_t c = buf[1 + i * 2] | (buf[2 + i * 2] << 8); if (!c) break; pn += (char)c; }
                cli_names[pos] = pn;
            }
        }
        if (buf[0] == STOC_HS_PLAYER_CHANGE && n >= 1) {
            int pos = buf[1] >> 4;
            int st = buf[1] & 0x0f;
            if (pos < 2) {
                if (st == PLAYERCHANGE_LEAVE) cli_names[pos].clear();
                cli_ready[pos] = (st == PLAYERCHANGE_READY);
            }
        }
    }
    return 0;
}

// Human-readable reason for a server rejection (STOC_ERROR_MSG msg/code).
static std::string cli_err_reason(uint32_t msg, uint32_t code) {
    char hex[16];
    switch (msg) {
    case ERRMSG_JOINERROR: return "room full or join rejected";
    case ERRMSG_DECKERROR:
        std::snprintf(hex, sizeof hex, "%08x", (unsigned)code);
        return "deck rejected (error 0x" + std::string(hex) + ")";
    case ERRMSG_SIDEERROR: return "side deck rejected";
    case ERRMSG_VERERROR: return "version mismatch (server expects " + std::to_string(code) + ")";
    default: return "server error (msg " + std::to_string(msg) + ")";
    }
}

// Drop the current network connection and reset ALL network-client state back
// to the main menu. Used by ygo_disconnect and every fatal/EOF cleanup path.
static void cli_net_reset() {
    if (net_client_fd >= 0) net::close(net_client_fd);
    net_client_fd = -1;
    net_client_connected = false;
    net::reset_eof();
    cli_clear_field();
    cli_deck_codes.clear();
    cli_game_over = false;
    cli_winner = -1;
    cli_stage = 0;
    cli_is_host = false;
    cli_ready[0] = cli_ready[1] = false;
    cli_names[0] = cli_names[1] = "";
    cli_pending_hand = cli_pending_tp = 0;
    cli_match_mode = false;
    cli_match_over = false;
}

// ygo_client MCP tool: connect/join a room (join-or-create by password).
// Returns the room lobby state + choices; drive with ygo_choose.
static std::string mcp_tool_ygo_client(const nlohmann::json& params) {
    if (!net_client_connected || net_client_fd < 0) {
        std::string host = params.value("host", "127.0.0.1");
        int port = params.value("port", 7911);
        nlohmann::json deck = params.contains("deck") ? params["deck"] : nlohmann::json(mcp_deck0_path);
        std::string name = params.value("name", "ygocli");
        std::string password = params.value("password", "");
        std::vector<uint32_t> codes;
        if (!resolve_deck_codes(deck, codes)) return "Error: invalid deck (need a .ydk path or array of card codes)";
        int rc = cli_connect_and_join(host, port, codes, name, password);
        if (rc != 0) {
            // Spec: on failure, return error and stop the connection (exit room).
            cli_net_reset();
            if (rc == -1) return "Error: cannot connect to server (" + host + ":" + std::to_string(port) + ")";
            return "Error: " + cli_err_reason(cli_last_err_msg, cli_last_err_code) + " - connection closed";
        }
        cli_clear_field();
    }

    // Room lobby view. Do NOT auto-ready or auto-start: the agent decides via
    // ygo_choose (mirrors the GUI host-prep room).
    std::ostringstream out;
    out << "Room: ";
    out << (cli_is_host ? "host (P" + std::to_string(net_client_seat) + ")" :
            "player P" + std::to_string(net_client_seat));
    if (cli_match_mode) out << " [match]";
    out << "\n";
    for (int i = 0; i < 2; ++i) {
        out << "  P" << i << ": " << (cli_names[i].empty() ? "(empty)" : cli_names[i])
            << (cli_ready[i] ? " [ready]" : "") << "\n";
    }
    out << cli_lobby_choices();
    return out.str();
}

// Room lobby choices (mirrors the GUI host-prep room buttons).
static std::string cli_lobby_choices() {
    std::ostringstream out;
    out << "0. ";
    bool is_observer = net_client_seat >= 2;
    if (is_observer) {
        out << "take a free seat (back to duelist)";
    } else if (cli_ready[net_client_seat]) {
        out << "cancel ready";
    } else {
        out << "ready with deck";
    }
    out << "\n";
    if (!is_observer && cli_is_host && cli_ready[0] && cli_ready[1]) {
        out << "1. start duel\n";
    }
    if (!is_observer && !cli_names[1 - net_client_seat].empty()) {
        out << "2. switch to observer\n";
    }
    out << "-1. wait\n";
    return out.str();
}

// Apply a room-lobby / RPS / TP choice and return the next view.
static std::string cli_apply_room_choice(int choice_idx) {
    if (cli_pending_hand) {
        cli_pending_hand = 0;
        if (choice_idx < 1 || choice_idx > 3) return "Error: RPS choice must be 1 (rock), 2 (paper) or 3 (scissors)";
        std::vector<uint8_t> rr = {(uint8_t)choice_idx};
        net::write_packet(net_client_fd, CTOS_HAND_RESULT, rr.data(), 1);
        return "RPS: played " + std::string(choice_idx == 1 ? "rock" : choice_idx == 2 ? "paper" : "scissors") + "\n";
    }
    if (cli_pending_tp) {
        cli_pending_tp = 0;
        if (choice_idx != 0 && choice_idx != 1) return "Error: TP choice must be 0 (first) or 1 (second)";
        std::vector<uint8_t> tp = {(uint8_t)choice_idx};
        net::write_packet(net_client_fd, CTOS_TP_RESULT, tp.data(), 1);
        return "TP: chose " + std::string(choice_idx == 0 ? "first" : "second") + "\n";
    }
    bool is_observer = net_client_seat >= 2;
    if (choice_idx == 0) {
        if (is_observer) {
            net::send_packet(net_client_fd, CTOS_HS_TODUELIST);
            return "Requested duelist seat\n";
        }
        if (cli_ready[net_client_seat]) {
            net::send_packet(net_client_fd, CTOS_HS_NOTREADY);
            return "Canceled ready\n";
        }
        // ready with deck: upload deck, then ready
        std::vector<uint8_t> dk = cli_build_deck_payload(cli_deck_codes);
        net::write_packet(net_client_fd, CTOS_UPDATE_DECK, dk.data(), dk.size());
        net::send_packet(net_client_fd, CTOS_HS_READY);
        cli_ready[net_client_seat] = true;
        return "Ready\n";
    }
    if (choice_idx == 1 && cli_is_host && cli_ready[0] && cli_ready[1]) {
        net::send_packet(net_client_fd, CTOS_HS_START);
        return "Started duel\n";
    }
    if (choice_idx == 2 && !is_observer && !cli_names[1 - net_client_seat].empty()) {
        net::send_packet(net_client_fd, CTOS_HS_TOOBSERVER);
        return "Switched to observer\n";
    }
    if (choice_idx == -1) return "";
    return "Error: invalid choice\n";
}

// ygo_disconnect: close the current network connection (leave the room).
static std::string mcp_tool_ygo_disconnect(const nlohmann::json&) {
    if (!net_client_connected || net_client_fd < 0) return "No active connection";
    cli_net_reset();
    return "Left the room (connection closed)";
}

// ygo_chat: send a chat message to the current room.
static std::string mcp_tool_ygo_chat(const nlohmann::json& params) {
    if (!net_client_connected || net_client_fd < 0) return "Error: not connected to a server";
    std::string text = params.value("text", "");
    if (text.empty()) return "Error: text parameter required";
    std::vector<uint8_t> ch(256 * 2, 0);
    utf16_write(ch, 0, text, 256);
    net::write_packet(net_client_fd, CTOS_CHAT, ch.data(), 512);
    return "Sent: " + text;
}

// ygo_surrender: send CTOS_SURRENDER and keep reading. In a match the server
// declares the opponent the winner, runs the side-deck phase, and starts the
// next game; in a single game it ends the duel.
static std::string mcp_tool_ygo_surrender(const nlohmann::json&) {
    if (!net_client_connected || net_client_fd < 0) return "Error: not connected to a server";
    net::send_packet(net_client_fd, CTOS_SURRENDER);
    net_client_mode = true;
    mcp_begin_capture();
    int r = cli_read_until_prompt(500);
    std::string narration = mcp_take_capture();
    mcp_end_capture();
    net_client_mode = false;
    std::ostringstream out;
    if (!narration.empty()) out << narration;
    out << cli_render_field();
    if (r == 2) {
        std::string err = cli_err_reason(cli_last_err_msg, cli_last_err_code);
        cli_net_reset();
        return "Error: " + err + " - connection closed";
    }
    if (r == 1) {
        if (cli_game_over) out << "You surrendered - Game Over: Player " << cli_winner << " wins!\n";
        else out << "You surrendered - Game Over (no winner)\n";
        return out.str();
    }
    out << "(surrendered, waiting for next game)\n";
    return out.str();
}

// ygo_observe: connect as observer (sees public info, never prompted).
static std::string mcp_tool_ygo_observe(const nlohmann::json& params) {
    if (!net_client_connected || net_client_fd < 0) {
        std::string host = params.value("host", "127.0.0.1");
        int port = params.value("port", 7911);
        std::string name = params.value("name", "observer");
        std::string password = params.value("password", "");
        // Observers send PLAYER_INFO + JOIN_GAME, then switch to observer role.
        net_client_fd = net::connect(host, port);
        if (net_client_fd < 0) return "Error: cannot connect to " + host + ":" + std::to_string(port);
        net_client_connected = true;
        std::vector<uint8_t> ni(40, 0);
        utf16_write(ni, 0, name, 20);
        net::write_packet(net_client_fd, CTOS_PLAYER_INFO, ni.data(), 40);
        std::vector<uint8_t> jg(sizeof(CTOS_JoinGame), 0);
        uint16_t ver = PRO_VERSION;
        std::memcpy(jg.data(), &ver, 2);
        utf16_write(jg, 8, password, 20);
        net::write_packet(net_client_fd, CTOS_JOIN_GAME, jg.data(), jg.size());
        std::vector<uint8_t> buf;
        bool got_type = false;
        while (!got_type) {
            int n = net::read_packet(net_client_fd, buf, 3000);
            if (n < 0) { cli_net_reset(); return "Error: handshake failed"; }
            if (buf.empty()) continue;
            if (buf[0] == STOC_ERROR_MSG && n >= 8) {
                std::memcpy(&cli_last_err_msg, buf.data() + 1, 4);
                std::memcpy(&cli_last_err_code, buf.data() + 5, 4);
                std::string err = cli_err_reason(cli_last_err_msg, cli_last_err_code);
                cli_net_reset();
                return "Error: " + err + " - connection closed";
            }
            if (buf[0] == STOC_TYPE_CHANGE && n >= 1) {
                net_client_seat = buf[1] & 0x0f;
                got_type = true;
            }
        }
        net::send_packet(net_client_fd, CTOS_HS_TOOBSERVER);
        cli_clear_field();
    }
    net_client_mode = true;
    mcp_begin_capture();
    int r = cli_read_until_prompt(500);
    std::string narration = mcp_take_capture();
    mcp_end_capture();
    net_client_mode = false;
    std::ostringstream out;
    if (!narration.empty()) out << narration;
    out << cli_render_field();
    if (r == 2) {
        std::string err = cli_err_reason(cli_last_err_msg, cli_last_err_code);
        cli_net_reset();
        return "Error: " + err + " - connection closed";
    }
    if (r == 1) {
        if (cli_game_over) out << "Game Over: Player " << cli_winner << " wins!\n";
        else out << "Game Over (no winner)\n";
        return out.str();
    }
    if (cli_has_prompt) { cli_has_prompt = false; out << "(observing...)\n"; return out.str(); }
    out << "(watching, waiting for action)\n";
    return out.str();
}

// ygo_replay: play back a saved .yrp replay (ygopro/gframe format).
// Format: magic "ygopro" (6) + version u32 + seed[8] u32, then chunks of
// [u32 len][len bytes] until EOF; each chunk is a batch of engine messages.
static std::string mcp_tool_ygo_replay(const nlohmann::json& params) {
    std::string file = params.value("file", "");
    if (file.empty()) return "Error: file parameter required";
    std::ifstream in(file, std::ios::binary);
    if (!in) return "Error: cannot open " + file;
    char magic[6];
    in.read(magic, 6);
    if (in.gcount() != 6 || std::memcmp(magic, "ygopro", 6) != 0)
        return "Error: not a ygopro replay file (bad magic)";
    uint32_t version = 0;
    in.read((char*)&version, 4);
    uint8_t seedbuf[32]{};
    in.read((char*)seedbuf, sizeof seedbuf);   // header seed (tolerate short files)

    // Fresh view state; the replay's own MSG_START re-initializes LP/decks.
    cli_clear_field();
    cli_turn = 1;
    cli_phase = 0x04;
    cli_lp[0] = cli_lp[1] = 8000;
    cli_game_over = false;
    cli_winner = -1;

    std::ostringstream events;
    int chunks = 0, turns = 0, damage = 0, summons = 0;
    while (in) {
        uint32_t len = 0;
        in.read((char*)&len, 4);
        if (!in || len == 0 || len > (1u << 20)) break;
        std::vector<uint8_t> data(len);
        in.read((char*)data.data(), len);
        if (in.gcount() != (std::streamsize)len) break;
        uint8_t* p = data.data();
        uint8_t* end = data.data() + len;
        while (p < end) {
            uint8_t mt = (p < end) ? *p : 0;
            // Lightweight timeline stats.
            if (mt == MSG_NEW_TURN) turns++;
            else if (mt == MSG_DAMAGE && p + 6 <= end) {
                int pl = p[1];
                int32_t d = 0;
                std::memcpy(&d, p + 2, 4);
                events << "  P" << pl << " takes " << d << " damage\n";
                damage++;
            } else if (mt == MSG_SUMMONING && p + 6 <= end) {
                uint32_t c = 0;
                std::memcpy(&c, p + 2, 4);
                events << "  P" << (int)p[1] << " summons " << get_card_name(c) << "\n";
                summons++;
            }
            int r = cli_handle_msg(p, end);
            if (r == 2) { cli_game_over = true; break; }
            if (cli_has_prompt) cli_has_prompt = false;   // replays are fixed; skip prompts
        }
        chunks++;
    }
    std::ostringstream out;
    out << "=== Replay: " << file << " (version 0x" << std::hex << version << std::dec
        << ", " << chunks << " chunks) ===\n";
    if (turns) out << "Turns played: " << turns << "; summons: " << summons << "; damage events: " << damage << "\n";
    if (!events.str().empty()) out << events.str();
    out << "--- final state ---\n" << cli_render_field();
    if (cli_game_over && cli_winner >= 0) out << "Result: Player " << cli_winner << " wins!\n";
    else out << "Result: (no MSG_WIN found in replay; file may be truncated)\n";
    return out.str();
}


int main(int argc, char* argv[]) {
    // Resolve the executable path/dir early (locating wiki/, single/, replay/).
    {
        char exe_path[PATH_MAX];
        if (realpath(argv[0], exe_path)) {
            g_exe_path = exe_path;
            size_t slash = g_exe_path.find_last_of('/');
            if (slash != std::string::npos) g_exe_dir = g_exe_path.substr(0, slash);
        }
    }

    // Subcommands: mcp | interact. Anything else = usage help.
    if (argc >= 2) {
        std::string cmd = argv[1];
        if (cmd == "interact") {
            load_card_database("./cards.cdb");
            load_strings_conf("./strings.conf");
            mcp_interact_loop();
            if (db) { sqlite3_close(db); db = nullptr; }
            return 0;
        }
        if (cmd == "mcp") { mcp_mode = true; }
    }

    if (!mcp_mode) {
        std::cerr << "Usage: " << argv[0] << " mcp <deck0.ydk> <deck1.ydk>   launch MCP server (JSON-RPC over stdio)\n";
        std::cerr << "       " << argv[0] << " interact                     interactive mode (same engine as MCP)\n";
        std::cerr << "Network games connect to a YGOPro server (e.g. srvpro).\n";
        return 1;
    }

    // In MCP mode, chdir to the executable's directory so the relative data
    // paths (cards.cdb, strings.conf, script/) resolve no matter what working
    // directory the MCP client launches us from.
    char cwdbuf[PATH_MAX];
    std::string orig_cwd = getcwd(cwdbuf, sizeof(cwdbuf)) ? cwdbuf : "";
    char exe_path[PATH_MAX];
    if (realpath(argv[0], exe_path)) {
        std::string dir = exe_path;
        size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos) {
            dir.resize(slash);
            if (!dir.empty()) chdir(dir.c_str());
        }
    }

    // Load card database
    load_card_database("./cards.cdb");
    load_strings_conf("./strings.conf");

    // Optional default decks from argv (the "mcp" subcommand is argv[1]);
    // resolve relative paths against the original cwd (we chdir'd above).
    if (argc >= 4) {
        std::string d0 = argv[2];
        std::string d1 = argv[3];
        if (!d0.empty() && d0[0] != '/' && !orig_cwd.empty()) d0 = orig_cwd + "/" + d0;
        if (!d1.empty() && d1[0] != '/' && !orig_cwd.empty()) d1 = orig_cwd + "/" + d1;
        mcp_deck0_path = d0;
        mcp_deck1_path = d1;
    }

    mcp_jsonrpc_loop();
    if (db) { sqlite3_close(db); db = nullptr; }
    return 0;
}
