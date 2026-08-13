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

std::string card_type_brief(uint32_t type) {
    std::vector<std::string> parts;
    static const std::pair<uint32_t, uint32_t> type_ids[] = {
        {TYPE_MONSTER, 1050},
        {TYPE_SPELL, 1051},
        {TYPE_TRAP, 1052},
        {TYPE_NORMAL, 1054},
        {TYPE_EFFECT, 1055},
        {TYPE_FUSION, 1056},
        {TYPE_RITUAL, 1057},
        {TYPE_SPIRIT, 1059},
        {TYPE_UNION, 1060},
        {TYPE_DUAL, 1061},
        {TYPE_TUNER, 1062},
        {TYPE_SYNCHRO, 1063},
        {TYPE_TOKEN, 1064},
        {TYPE_QUICKPLAY, 1066},
        {TYPE_CONTINUOUS, 1067},
        {TYPE_EQUIP, 1068},
        {TYPE_FIELD, 1069},
        {TYPE_COUNTER, 1070},
        {TYPE_FLIP, 1071},
        {TYPE_TOON, 1072},
        {TYPE_XYZ, 1073},
        {TYPE_PENDULUM, 1074},
        {TYPE_SPSUMMON, 1075},
        {TYPE_LINK, 1076},
    };
    for (const auto& [flag, sys_id] : type_ids) {
        if (type & flag) {
            parts.push_back(get_sys_string(sys_id));
        }
    }
    if (parts.empty()) {
        return get_sys_string(1053);
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "|";
        out += parts[i];
    }
    return out;
}

std::string get_card_bracket_info(uint32_t code) {
    auto dit = card_datas.find(code);
    std::string effect = normalize_effect_text(card_descs.count(code) ? card_descs[code] : "");
    if (effect.empty()) effect = "-";
    if (dit == card_datas.end()) return "(" + get_sys_string(1053) + " - -/- " + effect + ")";
    const auto& d = dit->second;
    uint32_t lv = d.level & 0xff;
    return "(" + card_type_brief(d.type)
        + " " + std::to_string(lv)
        + " " + std::to_string(d.attack)
        + "/" + std::to_string(d.defense)
        + " " + effect + ")";
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
bool auto_play = false;
bool random_choices = false;
bool mcp_mode = false;
intptr_t global_pduel = 0;
std::mt19937 choice_rng;
bool choice_rng_initialized = false;

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

// Last message cache for MSG_RETRY
std::vector<uint8_t> last_successful_msg;
size_t last_successful_msg_length = 0;

// Last response cache for MSG_RETRY
enum ResponseType { RESPONSE_NONE, RESPONSE_I, RESPONSE_B };
ResponseType last_response_type = RESPONSE_NONE;
int32_t last_response_i = 0;
std::vector<uint8_t> last_response_b;
size_t last_response_b_length = 0;
intptr_t last_pduel = 0;

// Helper to cache and set int response
void cache_and_set_responsei(intptr_t pduel, int32_t resp) {
    last_response_type = RESPONSE_I;
    last_response_i = resp;
    last_pduel = pduel;
    std::cout << "[DEBUG] Cached int response: " << resp << "\n";
    set_responsei(pduel, resp);
}

// Helper to cache and set buffer response
void cache_and_set_responseb(intptr_t pduel, unsigned char* resp, size_t len) {
    last_response_type = RESPONSE_B;
    last_response_b.resize(len);
    if (len > 0 && resp) {
        std::memcpy(last_response_b.data(), resp, len);
    }
    last_response_b_length = len;
    last_pduel = pduel;
    std::cout << "[DEBUG] Cached buffer response (length: " << len << "): ";
    for (size_t i = 0; i < len; i++) {
        std::cout << std::hex << (int)resp[i] << " ";
    }
    std::cout << std::dec << "\n";
    set_responseb(pduel, resp);
}

void send_selected_indices_response(intptr_t pduel, const std::vector<uint8_t>& indices) {
    std::vector<uint8_t> buf(indices.size() + 1);
    buf[0] = static_cast<uint8_t>(indices.size());
    for(size_t i = 0; i < indices.size(); ++i) {
        buf[i + 1] = indices[i];
    }
    cache_and_set_responseb(pduel, buf.data(), buf.size());
}

void init_choice_rng(uint32_t fallback_seed = 0) {
    const char* choice_seed_env = std::getenv("YGOCLI_CHOICE_SEED");
    uint32_t seed = fallback_seed;
    if (choice_seed_env && choice_seed_env[0] != '\0') {
        seed = static_cast<uint32_t>(std::strtoul(choice_seed_env, nullptr, 10));
    } else if (seed == 0) {
        std::random_device rd;
        seed = rd();
    }

    choice_rng.seed(seed);
    choice_rng_initialized = true;
    if (random_choices) {
        std::cout << "[INFO] Random choice mode enabled (choice seed: " << seed << ")\n";
    }
}

void print_core_error(const std::string& message) {
    if (message.empty()) {
        return;
    }
    const char* red = "\033[31m";
    const char* reset = "\033[0m";
    std::cerr << red << "[CORE] " << message << reset << "\n";
}

int rand_int_inclusive(int lo, int hi) {
    if (hi < lo) return lo;
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(choice_rng);
}

std::vector<uint8_t> random_unique_indices(int count, int take) {
    std::vector<uint8_t> pool;
    pool.reserve(std::max(0, count));
    for (int i = 0; i < count; ++i) {
        pool.push_back(static_cast<uint8_t>(i));
    }
    std::shuffle(pool.begin(), pool.end(), choice_rng);
    if (take < 0) take = 0;
    if (take > count) take = count;
    pool.resize(static_cast<size_t>(take));
    return pool;
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
    // Setcode
    bool has_set = false;
    for (int i = 0; i < SIZE_SETCODE; i++) {
        if (d.setcode[i]) { has_set = true; break; }
    }
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

void display_location_cards(int player, uint32_t location, const char* label, bool show_details = false) {
    uint8_t buffer[8192];
    uint32_t query_flags = QUERY_CODE | QUERY_POSITION;
    if (show_details) {
        query_flags |= QUERY_ATTACK | QUERY_DEFENSE | QUERY_LEVEL;
    }
    int len = query_field_card(global_pduel, player, location, query_flags, buffer, 0);
    if (len > 0) {
        uint8_t* p = buffer;
        int seq = 0;
        while (p - buffer < len) {
            uint8_t* card_start = p;
            int32_t card_len = BufferIO::Read<int32_t>(p);
            //std::cout << "DEBUG: card " << seq << " len=" << card_len << "\n";
            if (card_len > 4) {
                uint8_t* card_p = p;
                int32_t flag = BufferIO::Read<int32_t>(card_p);
                uint32_t code = 0;
                uint8_t pos = 0;
                int32_t atk = 0;
                int32_t def = 0;
                uint32_t lv = 0;

                if (flag & QUERY_CODE) {
                    code = BufferIO::Read<uint32_t>(card_p);
                }
                if (flag & QUERY_POSITION) {
                    uint32_t posval = BufferIO::Read<uint32_t>(card_p);
                    pos = (posval >> 24) & 0xff;
                }
                if (flag & QUERY_ALIAS) {
                    BufferIO::Read<uint32_t>(card_p);
                }
                if (flag & QUERY_TYPE) {
                    BufferIO::Read<uint32_t>(card_p);
                }
                if (flag & QUERY_LEVEL) {
                    lv = BufferIO::Read<uint32_t>(card_p);
                }
                if (flag & QUERY_RANK) {
                    BufferIO::Read<uint32_t>(card_p);
                }
                if (flag & QUERY_ATTRIBUTE) {
                    BufferIO::Read<uint32_t>(card_p);
                }
                if (flag & QUERY_RACE) {
                    BufferIO::Read<uint32_t>(card_p);
                }
                if (flag & QUERY_ATTACK) {
                    atk = BufferIO::Read<int32_t>(card_p);
                }
                if (flag & QUERY_DEFENSE) {
                    def = BufferIO::Read<int32_t>(card_p);
                }

                if (code != 0) {
                    std::cout << "    [" << seq << "] " << get_card_name(code);
                    std::cout << " " << get_card_bracket_info(code);
                    // Only show position for field cards (MZone/SZone), not hand/deck
                    if (pos != 0 && show_details) {
                        std::cout << " " << pos_name(pos);
                    }
                    std::cout << "\n";
                }
            }
            p = card_start + card_len;
            seq++;
        }
    }
}

void display_game_state() {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "GAME STATE - Turn: " << turn << " - Phase: " << phase_name(phase) << "\n";
    std::cout << std::string(60, '=') << "\n";

    for (int player = 0; player < 2; ++player) {
        std::cout << "=== Player " << player << " - LP: " << lp[player] << "\n";

        // Deck and Extra just show counts using query_field_count
        int deck_count = query_field_count(global_pduel, player, LOCATION_DECK);
        std::cout << "  Deck: " << deck_count << " cards\n";

        int extra_count = query_field_count(global_pduel, player, LOCATION_EXTRA);
        std::cout << "  Extra: " << extra_count << " cards\n";

        // Hand - show all cards
        std::cout << "  Hand:\n";
        display_location_cards(player, LOCATION_HAND, "Hand");

        // Grave - show all cards
        std::cout << "  Grave:\n";
        display_location_cards(player, LOCATION_GRAVE, "Grave");

        // Removed - show all cards
        std::cout << "  Removed:\n";
        display_location_cards(player, LOCATION_REMOVED, "Removed");

        // MZone - show cards with lv/atk/def
        std::cout << "  MZone:\n";
        display_location_cards(player, LOCATION_MZONE, "MZone", true);

        // SZone - show cards
        std::cout << "  SZone:\n";
        display_location_cards(player, LOCATION_SZONE, "SZone");
    }
    std::cout << std::string(60, '=') << "\n\n";
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

// Run the engine until a prompt is encountered or the game ends.
// Narration goes to std::cout (captured by mcp_capture).
// On prompt: stores prompt info, returns 0.
// On game end: returns 1 (mcp_winner/mcp_win_reason set).
// Call mcp_send_choice() before calling this again to respond to the prompt.
int mcp_run_until_pause() {
    if (global_pduel == 0) return -1;
    intptr_t pduel = global_pduel;

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
                        (void)prev_ctrl;(void)prev_pos;(void)new_pos;
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
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        (void)pl;(void)loc;(void)seq;
                        std::cout << "Summoning " << get_card_name(code) << "\n";
                        break;
                    }
                    case MSG_SPSUMMONING: {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        (void)pl;(void)loc;(void)seq;
                        std::cout << "Special Summoning " << get_card_name(code) << "\n";
                        break;
                    }
                    case MSG_FLIPSUMMONING: {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        (void)pl;(void)loc;(void)seq;
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
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        std::cout << "Chained: " << get_card_name(code) << "\n";
                        mcp_mark_important(code);
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
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        (void)pl;(void)loc;(void)seq;
                        std::cout << "Targets " << get_card_name(code) << "\n";
                        break;
                    }
                    case MSG_CANCEL_TARGET: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        (void)pl;(void)loc;(void)seq;
                        std::cout << "Cancel target " << get_card_name(code) << "\n";
                        break;
                    }
                    case MSG_EQUIP: {
                        uint8_t eq_ctrl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t eq_loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t eq_seq = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t tgt_ctrl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t tgt_loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t tgt_seq = BufferIO::Read<uint8_t>(pbuf);
                        (void)eq_ctrl;(void)eq_loc;(void)eq_seq;(void)tgt_ctrl;(void)tgt_loc;(void)tgt_seq;
                        std::cout << "Equip: " << location_name(eq_loc) << "[" << (int)eq_seq << "]"
                                  << " -> " << location_name(tgt_loc) << "[" << (int)tgt_seq << "]\n";
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
                        uint8_t cnt = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "Random selected: ";
                        for (int i = 0; i < cnt; i++) {
                            uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                            uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                            uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                            (void)pl;(void)loc;(void)seq;
                            std::cout << (int)seq << " ";
                        }
                        std::cout << "\n";
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
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t desc = BufferIO::Read<uint32_t>(pbuf);
                        (void)pl;(void)loc;(void)seq;(void)desc;
                        std::cout << "Missed effect: " << get_card_name(code) << "\n";
                        break;
                    }
                    case MSG_TAG_SWAP: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        (void)pl;
                        uint16_t count1 = BufferIO::Read<uint16_t>(pbuf);
                        uint16_t count2 = BufferIO::Read<uint16_t>(pbuf);
                        pbuf += count1 * 4 + count2 * 4 + 7;
                        std::cout << "Tag swap.\n";
                        break;
                    }
                    case MSG_RELOAD_FIELD: {
                        std::cout << "Field reloaded.\n";
                        break;
                    }
                    case MSG_CARD_HINT: {
                        uint8_t pl = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t hint_type = BufferIO::Read<uint8_t>(pbuf);
                        (void)pl;(void)loc;(void)seq;(void)hint_type;
                        std::cout << "Card hint: " << get_card_name(code) << "\n";
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

// Position byte of the card at (player, loc, seq), 0 if none/not found.
uint8_t mcp_query_pos(int player, uint32_t loc, int seq) {
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
    auto rd_pt = [&]() { rd32(); rd8(); rd8(); rd8(); };              // 7 bytes
    auto rd_act = [&]() { rd32(); rd8(); rd8(); rd8(); rd32(); };     // 11 bytes
    auto rd_card9 = [&]() { rd32(); rd8(); rd8(); rd8(); rd8(); };    // 8 bytes (code+4)

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
            uint8_t edesc = rd8();
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
        rd8(); // skip player byte
        rd8(); // count
        uint32_t forbidden = rd32();
        uint32_t sel = ~forbidden;
        for (int i=0;i<7;i++) if (sel & (1u<<i)) out << idx++ << ": P0 MZone["<<i<<"]\n";
        for (int i=0;i<8;i++) if (sel & (0x100u<<i)) out << idx++ << ": P0 SZone["<<i<<"]\n";
        for (int i=0;i<7;i++) if (sel & (0x10000u<<i)) out << idx++ << ": P1 MZone["<<i<<"]\n";
        for (int i=0;i<8;i++) if (sel & (0x1000000u<<i)) out << idx++ << ": P1 SZone["<<i<<"]\n";
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
    if (pduel == 0) return;
    int type = mcp_pending_prompt.msg_type;

    // Re-read stored data for response construction
    uint8_t* p = const_cast<uint8_t*>(mcp_pending_prompt.msg_data.data());
    uint8_t* end = p + mcp_pending_prompt.msg_data.size();

    auto rd8  = [&]() -> uint8_t  { if (p < end) return *p++; return 0; };
    auto rd16 = [&]() -> uint16_t { uint16_t v=0; if (p+2<=end) std::memcpy(&v,p,2); p+=2; return v; };
    auto rd32 = [&]() -> uint32_t { uint32_t v=0; if (p+4<=end) std::memcpy(&v,p,4); p+=4; return v; };

    auto send_int = [&](int32_t val) {
        mcp_last_response.valid = true;
        mcp_last_response.is_buffer = false;
        mcp_last_response.int_val = val;
        set_responsei(pduel, val);
    };
    auto send_buf = [&](const std::vector<uint8_t>& buf) {
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
            rd8(); // skip player byte
            rd8(); // count
            uint32_t forbidden = rd32();
            uint32_t sel = ~forbidden;
            // Build the same options list as mcp_build_choices
            struct PlaceOpt { uint8_t ctrl,loc,seq; };
            std::vector<PlaceOpt> opts;
            for (int i=0;i<7;i++) if (sel & (1u<<i)) opts.push_back({0,(uint8_t)LOCATION_MZONE,(uint8_t)i});
            for (int i=0;i<8;i++) if (sel & (0x100u<<i)) opts.push_back({0,(uint8_t)LOCATION_SZONE,(uint8_t)i});
            for (int i=0;i<7;i++) if (sel & (0x10000u<<i)) opts.push_back({1,(uint8_t)LOCATION_MZONE,(uint8_t)i});
            for (int i=0;i<8;i++) if (sel & (0x1000000u<<i)) opts.push_back({1,(uint8_t)LOCATION_SZONE,(uint8_t)i});
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
// Handles initialize, tools/list, and tools/call (ygo_start, ygo_choose, ygo_card_search)

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

// Register engine callbacks (card data + scripts + logs). The CLI does this at
// startup; MCP mode needs it too, otherwise the engine has no card data and
// nothing is summonable.
static void mcp_setup_engine() {
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
        if (requested.rfind("./script/", 0) == 0) {
            path = requested;
        } else if (requested.rfind("script/", 0) == 0) {
            path = "./" + requested;
        } else {
            path = "./script/" + requested;
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

// Initialize a duel (reused by ygo_start and the initial setup)
static bool mcp_init_duel(const std::string& deck0_path, const std::string& deck1_path,
                          int start_lp, int start_hand, int draw_count, int rule) {
    // Clean up existing duel
    if (global_pduel != 0) {
        end_duel(global_pduel);
        global_pduel = 0;
        last_successful_msg.clear();
        last_successful_msg_length = 0;
        last_response_type = RESPONSE_NONE;
        last_response_b.clear();
        last_response_b_length = 0;
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

    start_duel(pduel, DUEL_SIMPLE_AI | ((uint32_t)rule << 16));
    mcp_engine_buffer.resize(SIZE_MESSAGE_BUFFER);
    mcp_reset_game_state();
    return true;
}

// ygo_start tool: init new game, run until choice
static std::string mcp_tool_ygo_start(const nlohmann::json& params) {
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

// ygo_choose tool: send choice, continue game
static std::string mcp_tool_ygo_choose(const nlohmann::json& params) {
    if (global_pduel == 0) return "Error: No active game";

    int choice_idx = -1;
    std::vector<int> choice_indices;
    if (params.contains("id") && params["id"].is_number()) choice_idx = params["id"];
    if (params.contains("indices") && params["indices"].is_array()) {
        for (const auto& v : params["indices"])
            if (v.is_number()) choice_indices.push_back(v);
    }

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
            // Echo the client's requested protocol version (MCP negotiation).
            nlohmann::json init_params = req.value("params", nlohmann::json::object());
            std::string pv = init_params.value("protocolVersion", "2024-11-05");
            resp["result"] = {
                {"protocolVersion", pv},
                {"serverInfo", {{"name", "ygocli"}, {"version", "1.0"}}},
                {"capabilities", {{"tools", nlohmann::json::object()}}}
            };
        } else if (method == "tools/list") {
            nlohmann::json tools = nlohmann::json::array();
            tools.push_back({
                {"name", "ygo_start"},
                {"description", "Start a new Yu-Gi-Oh! duel. Runs until a choice is needed."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"deck0", {{"type", "string"}, {"description", "Path to deck0 .ydk file"}}},
                        {"deck1", {{"type", "string"}, {"description", "Path to deck1 .ydk file"}}},
                        {"lp", {{"type", "integer"}, {"description", "Starting LP (default 8000)"}}},
                        {"start_hand", {{"type", "integer"}, {"description", "Starting hand size (default 5)"}}},
                        {"draw_count", {{"type", "integer"}, {"description", "Cards drawn per turn (default 1)"}}},
                        {"rule", {{"type", "integer"}, {"description", "Master rule version"}}}
                    }},
                    {"required", {"deck0", "deck1"}}
                }}
            });
            tools.push_back({
                {"name", "ygo_choose"},
                {"description", "Choose an option from the pending prompt and continue the duel."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"id", {{"type", "integer"}, {"description", "The choice index to select"}}},
                        {"indices", {{"type", "array"}, {"items", {{"type", "integer"}}},
                                     {"description", "For multi-select prompts, list of indices"}}}
                    }},
                    {"required", nlohmann::json::array()}
                }}
            });
            tools.push_back({
                {"name", "ygo_card_search"},
                {"description", "Search card database. AND across all params."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"id", {{"type", "integer"}}}, {"text", {{"type", "string"}}},
                        {"level", {{"type", "string"}}}, {"atk", {{"type", "string"}}},
                        {"def", {{"type", "string"}}}, {"type", {{"type", "string"}}},
                        {"race", {{"type", "string"}}}, {"attribute", {{"type", "string"}}}
                    }}
                }}
            });
            resp["result"]["tools"] = tools;
        } else if (method == "tools/call") {
            nlohmann::json params = req.value("params", nlohmann::json::object());
            std::string tool_name = params.value("name", "");
            nlohmann::json args = params.value("arguments", nlohmann::json::object());
            if (!args.is_object()) args = nlohmann::json::object();

            std::string tool_result;
            if (tool_name == "ygo_start") {
                tool_result = mcp_tool_ygo_start(args);
            } else if (tool_name == "ygo_choose") {
                tool_result = mcp_tool_ygo_choose(args);
            } else if (tool_name == "ygo_card_search") {
                tool_result = mcp_tool_card_search(args);
            } else {
                error_code = -32601;
                error_msg = "Unknown tool: " + tool_name;
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

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <deck0.ydk> <deck1.ydk> [--auto]\n";
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--auto") {
            auto_play = true;
        } else if (std::string(argv[i]) == "--random") {
            auto_play = true;
            random_choices = true;
        } else if (std::string(argv[i]) == "--mcp") {
            mcp_mode = true;
        }
    }
    const char* random_choices_env = std::getenv("YGOCLI_RANDOM_CHOICES");
    if (random_choices_env && random_choices_env[0] != '\0' && std::string(random_choices_env) != "0") {
        random_choices = true;
        auto_play = true;
    }

    // In MCP mode, chdir to the executable's directory so the relative data
    // paths (cards.cdb, strings.conf, script/) resolve no matter what working
    // directory the MCP client launches us from.
    std::string orig_cwd;
    if (mcp_mode) {
        char cwdbuf[PATH_MAX];
        if (getcwd(cwdbuf, sizeof(cwdbuf))) orig_cwd = cwdbuf;
        char exe_path[PATH_MAX];
        if (realpath(argv[0], exe_path)) {
            std::string dir = exe_path;
            size_t slash = dir.find_last_of('/');
            if (slash != std::string::npos) {
                dir.resize(slash);
                if (!dir.empty()) chdir(dir.c_str());
            }
        }
    }

    // Load card database
    load_card_database("./cards.cdb");
    load_strings_conf("./strings.conf");

    // MCP mode: JSON-RPC 2.0 server loop
    if (mcp_mode) {
        // Find deck paths from argv (skip flags); resolve relative paths against
        // the original cwd (we chdir'd to the executable dir above).
        std::vector<std::string> deck_paths;
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--mcp" || a == "--auto" || a == "--random") continue;
            if (!a.empty() && a[0] != '/' && !orig_cwd.empty())
                a = orig_cwd + "/" + a;
            deck_paths.push_back(a);
        }
        if (deck_paths.size() >= 2) {
            mcp_deck0_path = deck_paths[0];
            mcp_deck1_path = deck_paths[1];
        }
        mcp_jsonrpc_loop();
        if (db) { sqlite3_close(db); db = nullptr; }
        return 0;
    }

    // Load decks (find .ydk paths from argv, skipping flags)
    std::vector<std::string> deck_paths;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--auto" || a == "--random") continue;
        deck_paths.push_back(a);
    }
    Deck decks[2];
    if (deck_paths.size() >= 2) {
        decks[0] = load_deck(deck_paths[0]);
        decks[1] = load_deck(deck_paths[1]);
    }

    if (decks[0].main.empty() || decks[1].main.empty()) {
        std::cerr << "Invalid decks\n";
        return 1;
    }

    std::cout << "Deck 0: " << decks[0].main.size() << " main, " << decks[0].extra.size() << " extra\n";
    std::cout << "Deck 1: " << decks[1].main.size() << " main, " << decks[1].extra.size() << " extra\n";

    // Optional deterministic seed for reproducible tests/runs
    const char* fixed_seed_env = std::getenv("YGOCLI_SEED");
    bool has_fixed_seed = (fixed_seed_env != nullptr && fixed_seed_env[0] != '\0');
    uint32_t fixed_seed_value = 0;
    if (has_fixed_seed) {
        fixed_seed_value = static_cast<uint32_t>(std::strtoul(fixed_seed_env, nullptr, 10));
        std::cout << "[INFO] Using fixed seed: " << fixed_seed_value << "\n";
    }
    init_choice_rng(has_fixed_seed ? (fixed_seed_value ^ 0xa5a5a5a5U) : 0U);

    // Shuffle both decks and extra decks (engine does not shuffle when using new_card API)
    std::mt19937 rng_shuffle;
    if (has_fixed_seed) {
        rng_shuffle.seed(fixed_seed_value ^ 0x9e3779b9U);
    } else {
        std::random_device rd_shuffle;
        rng_shuffle.seed(rd_shuffle());
    }
    for (int i = 0; i < 2; i++) {
        std::shuffle(decks[i].main.begin(), decks[i].main.end(), rng_shuffle);
        std::shuffle(decks[i].extra.begin(), decks[i].extra.end(), rng_shuffle);
    }

    // Initialize ocgcore
    uint32_t seed[SEED_COUNT];
    if (has_fixed_seed) {
        std::mt19937 seed_rng(fixed_seed_value);
        for (int i = 0; i < SEED_COUNT; i++) {
            seed[i] = seed_rng();
        }
    } else {
        std::random_device rd;
        for (int i = 0; i < SEED_COUNT; i++) {
            seed[i] = rd();
        }
    }

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
        if (requested.rfind("./script/", 0) == 0) {
            path = requested;
        } else if (requested.rfind("script/", 0) == 0) {
            path = "./" + requested;
        } else {
            path = "./script/" + requested;
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

    intptr_t pduel = create_duel_v2(seed);
    global_pduel = pduel;
    set_player_info(pduel, 0, 8000, 5, 1);
    set_player_info(pduel, 1, 8000, 5, 1);

    // Add cards to duel
    for (int i = 0; i < 2; i++) {
        for (size_t j = 0; j < decks[i].main.size(); j++) {
            new_card(pduel, decks[i].main[j], i, i, LOCATION_DECK, decks[i].main.size() - 1 - j, POS_FACEDOWN_DEFENSE);
        }
        for (size_t j = 0; j < decks[i].extra.size(); j++) {
            new_card(pduel, decks[i].extra[j], i, i, LOCATION_EXTRA, j, POS_FACEDOWN_DEFENSE);
        }
    }

    // Start duel with master rule in upper 16 bits
    start_duel(pduel, DUEL_SIMPLE_AI | ((uint32_t)CURRENT_RULE << 16));

    // Engine buffer
    std::vector<uint8_t> engine_buffer(SIZE_MESSAGE_BUFFER);

    // Last message cache
    last_successful_msg.resize(SIZE_MESSAGE_BUFFER);

    // Main loop
    bool is_running = true;
    while (is_running) {
        uint32_t result = process(pduel);
        if (result & PROCESSOR_END) {
            std::cout << "\n[PROCESSOR_END] Duel ended!\n";
            is_running = false;
        }

        uint32_t len = result & PROCESSOR_BUFFER_LEN;

        if (len > 0) {
            get_message(pduel, engine_buffer.data());
            uint8_t* pbuf = engine_buffer.data();
            uint8_t* msg_buffer = engine_buffer.data();

            // Inner while loop to process multiple messages from one buffer
            while (pbuf - msg_buffer < (int)len) {
                uint8_t* msg_start = pbuf;

                uint8_t* offset = pbuf;
                uint8_t msg_type = BufferIO::Read<uint8_t>(pbuf);

                // Cache message if not retry
                if (msg_type != MSG_RETRY) {
                    size_t msg_len = len - (offset - msg_buffer);
                    if (msg_len > last_successful_msg.size()) {
                        last_successful_msg.resize(msg_len);
                    }
                    std::memcpy(last_successful_msg.data(), offset, msg_len);
                    last_successful_msg_length = msg_len;
                }

                bool needs_display_after = false;
                bool display_state_before = false;

                // First, print the message type
                std::cout << "[" << msg_type_name(msg_type) << "] ";

                switch (msg_type) {
                case MSG_RETRY: {
                    std::cout << "=== MSG_RETRY ===\n";
                    std::cout << "Last response type: ";
                    switch (last_response_type) {
                        case RESPONSE_NONE: std::cout << "RESPONSE_NONE"; break;
                        case RESPONSE_I: std::cout << "RESPONSE_I (value: " << last_response_i << ")"; break;
                        case RESPONSE_B: std::cout << "RESPONSE_B (length: " << last_response_b_length << ")"; break;
                    }
                    std::cout << "\nLast pduel: " << last_pduel << ", Current pduel: " << pduel << "\n";
                    if (last_response_type != RESPONSE_NONE && last_pduel == pduel) {
                        std::cout << "Retry requested; waiting for the prompt again.\n";
                    } else {
                        std::cout << "ERROR: No cached response to retry or pduel mismatch\n";
                    }
                    last_response_type = RESPONSE_NONE;
                    last_response_b.clear();
                    last_response_b_length = 0;
                    break;
                }
                case MSG_WIN: {
                    uint8_t winner = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t reason = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)winner << " wins! (reason: " << (int)reason << ")\n";
                    display_game_state();
                    is_running = false;
                    break;
                }
                case MSG_NEW_TURN: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    turn++;
                    std::cout << "Turn " << turn << ", Player " << (int)player << "'s turn\n";
                    break;
                }
               case MSG_NEW_PHASE: {
                    phase = BufferIO::Read<uint16_t>(pbuf);
                    std::cout << "Phase: " << phase_name(phase) << " (raw=0x" << std::hex << phase << std::dec << ")\n";
                    break;
                }
                case MSG_LPUPDATE: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    int32_t new_lp = BufferIO::Read<int32_t>(pbuf);
                    lp[player] = new_lp;
                    std::cout << "Player " << (int)player << " LP: " << new_lp << "\n";
                    break;
                }
                case MSG_DAMAGE: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    int32_t damage = BufferIO::Read<int32_t>(pbuf);
                    lp[player] -= damage;
                    if (lp[player] < 0) lp[player] = 0;
                    std::cout << "Player " << (int)player << " takes " << damage << " damage (LP: " << lp[player] << ")\n";
                    break;
                }
                case MSG_RECOVER: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    int32_t recover = BufferIO::Read<int32_t>(pbuf);
                    lp[player] += recover;
                    std::cout << "Player " << (int)player << " recovers " << recover << " LP (LP: " << lp[player] << ")\n";
                    break;
                }
                case MSG_PAY_LPCOST: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    int32_t cost = BufferIO::Read<int32_t>(pbuf);
                    lp[player] -= cost;
                    if (lp[player] < 0) lp[player] = 0;
                    std::cout << "Player " << (int)player << " pays " << cost << " LP (LP: " << lp[player] << ")\n";
                    break;
                }
                case MSG_DRAW: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " draws " << (int)count << " cards: ";
                    for(int i = 0; i < count; i++) {
                        uint32_t code = BufferIO::Read<int32_t>(pbuf);
                        if (i > 0) std::cout << ", ";
                        std::cout << get_card_name(code);
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
                    (void)prev_ctrl;
                    (void)prev_pos;
                    (void)new_pos;
                    std::cout << get_card_name(code) << " moved: "
                              << location_name(prev_loc) << "[" << (int)prev_seq << "]"
                              << " -> " << location_name(new_loc) << "[" << (int)new_seq << "]"
                              << " (player " << (int)new_ctrl << ", reason=" << reason << ")\n";
                    needs_display_after = true;
                    break;
                }
                case MSG_ATTACK: {
                    uint8_t attacker_player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t attacker_loc = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t attacker_seq = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t attacker_pos = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t target_player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t target_loc = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t target_seq = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t target_pos = BufferIO::Read<uint8_t>(pbuf);
                    (void)attacker_pos;
                    (void)target_pos;
                    std::cout << "Attack: Player " << (int)attacker_player
                              << " " << location_name(attacker_loc) << "[" << (int)attacker_seq << "]"
                              << " -> Player " << (int)target_player;
                    if (target_loc == 0) {
                        std::cout << " (direct attack)";
                    } else {
                        std::cout << " " << location_name(target_loc) << "[" << (int)target_seq << "]";
                    }
                    std::cout << "\n";
                    break;
                }
                case MSG_BATTLE: {
                    uint8_t attacker_player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t attacker_loc = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t attacker_seq = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t attacker_pos = BufferIO::Read<uint8_t>(pbuf);
                    int32_t attacker_atk = BufferIO::Read<int32_t>(pbuf);
                    int32_t attacker_def = BufferIO::Read<int32_t>(pbuf);
                    uint8_t attacker_destroyed = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t defender_player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t defender_loc = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t defender_seq = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t defender_pos = BufferIO::Read<uint8_t>(pbuf);
                    int32_t defender_atk = BufferIO::Read<int32_t>(pbuf);
                    int32_t defender_def = BufferIO::Read<int32_t>(pbuf);
                    uint8_t defender_destroyed = BufferIO::Read<uint8_t>(pbuf);
                    (void)attacker_pos;
                    (void)defender_pos;
                    std::cout << "Battle: P" << (int)attacker_player << " "
                              << location_name(attacker_loc) << "[" << (int)attacker_seq << "]"
                              << " (ATK:" << attacker_atk << "/DEF:" << attacker_def
                              << ", destroyed=" << (int)attacker_destroyed << ") vs "
                              << "P" << (int)defender_player << " ";
                    if(defender_loc == 0) {
                        std::cout << "(direct)";
                    } else {
                        std::cout << location_name(defender_loc) << "[" << (int)defender_seq << "]";
                    }
                    std::cout << " (ATK:" << defender_atk << "/DEF:" << defender_def
                              << ", destroyed=" << (int)defender_destroyed << ")\n";
                    break;
                }
                case MSG_POS_CHANGE: {
                    uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t location = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t sequence = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t prev_pos = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t new_pos = BufferIO::Read<uint8_t>(pbuf);
                    (void)prev_pos;
                    std::cout << get_card_name(code) << " position changed: "
                              << location_name(location) << "[" << (int)sequence << "]"
                              << " -> " << pos_name(new_pos) << "\n";
                    break;
                }
                case MSG_SUMMONED:
                case MSG_SPSUMMONED:
                case MSG_FLIPSUMMONED: {
                    needs_display_after = true;
                    std::cout << "Summon successful\n";
                    break;
                }
                case MSG_CHAINING: {
                    uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t location = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t sequence = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t sub_seq = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t trigger_player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t trigger_location = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t trigger_sequence = BufferIO::Read<uint8_t>(pbuf);
                    uint32_t desc = BufferIO::Read<uint32_t>(pbuf);
                    uint8_t chain_count = BufferIO::Read<uint8_t>(pbuf);
                    (void)sub_seq;
                    (void)trigger_player;
                    (void)trigger_location;
                    (void)trigger_sequence;
                    std::cout << "Chaining: " << get_card_name(code)
                              << " (" << location_name(location) << "[" << (int)sequence << "])"
                              << " desc=" << desc << " chain=" << (int)chain_count << "\n";
                    break;
                }
                case MSG_SELECT_IDLECMD: {
                    display_state_before = true;
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    (void)player;

                    // Save position to re-parse later
                    uint8_t* pbuf_start = pbuf;

                    uint8_t count;
                    int total_options = 0;

                    // First pass: count options AND FIND END POSITION
                    // Summon
                    count = BufferIO::Read<uint8_t>(pbuf);
                    total_options += count;
                    for (int i = 0; i < count; ++i) {
                        BufferIO::Read<int32_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                    }

                    // SP Summon
                    count = BufferIO::Read<uint8_t>(pbuf);
                    total_options += count;
                    for (int i = 0; i < count; ++i) {
                        BufferIO::Read<int32_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                    }

                    // Reposition
                    count = BufferIO::Read<uint8_t>(pbuf);
                    total_options += count;
                    for (int i = 0; i < count; ++i) {
                        BufferIO::Read<int32_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                    }

                    // MSet
                    count = BufferIO::Read<uint8_t>(pbuf);
                    total_options += count;
                    for (int i = 0; i < count; ++i) {
                        BufferIO::Read<int32_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                    }

                    // SSet
                    count = BufferIO::Read<uint8_t>(pbuf);
                    total_options += count;
                    for (int i = 0; i < count; ++i) {
                        BufferIO::Read<int32_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                    }

                    // Activate: each is 11 bytes {code:int32, ctrl:u8, loc:u8, seq:u8, desc:int32}
                    count = BufferIO::Read<uint8_t>(pbuf);
                    total_options += count;
                    for (int i = 0; i < count; ++i) {
                        BufferIO::Read<int32_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<int32_t>(pbuf);
                    }

                    uint8_t to_bp = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t to_ep = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t shuffle = BufferIO::Read<uint8_t>(pbuf);
                    (void)shuffle;

                    if (to_bp) total_options++;
                    if (to_ep) total_options++;

                    // SAVE END POSITION NOW!
                    uint8_t* pbuf_end = pbuf;

                    std::cout << "Player " << (int)player << " idle command, " << total_options << " options\n";

                    // Display game state BEFORE options
                    if (!auto_play) {
                        display_game_state();
                    }

                    // Second pass: display options
                    pbuf = pbuf_start;
                    int option_idx = 0;

                    if (!auto_play) {
                        // Summon
                        count = BufferIO::Read<uint8_t>(pbuf);
                        for (int i = 0; i < count; ++i) {
                            uint32_t code = BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            std::cout << "  [" << option_idx << "] Summon " << get_card_name(code) << "\n";
                            option_idx++;
                        }

                        // SP Summon
                        count = BufferIO::Read<uint8_t>(pbuf);
                        for (int i = 0; i < count; ++i) {
                            uint32_t code = BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            std::cout << "  [" << option_idx << "] SP Summon " << get_card_name(code) << "\n";
                            option_idx++;
                        }

                        // Reposition
                        count = BufferIO::Read<uint8_t>(pbuf);
                        for (int i = 0; i < count; ++i) {
                            uint32_t code = BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            std::cout << "  [" << option_idx << "] Reposition " << get_card_name(code) << "\n";
                            option_idx++;
                        }

                        // MSet
                        count = BufferIO::Read<uint8_t>(pbuf);
                        for (int i = 0; i < count; ++i) {
                            uint32_t code = BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            std::cout << "  [" << option_idx << "] MSet " << get_card_name(code) << "\n";
                            option_idx++;
                        }

                        // SSet
                        count = BufferIO::Read<uint8_t>(pbuf);
                        for (int i = 0; i < count; ++i) {
                            uint32_t code = BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            std::cout << "  [" << option_idx << "] SSet " << get_card_name(code) << "\n";
                            option_idx++;
                        }

                        // Activate
                        count = BufferIO::Read<uint8_t>(pbuf);
                        for (int i = 0; i < count; ++i) {
                            uint32_t code = BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<int32_t>(pbuf);
                            std::cout << "  [" << option_idx << "] Activate " << get_card_name(code) << "\n";
                            option_idx++;
                        }

                        // to_bp and to_ep
                        if (to_bp) {
                            std::cout << "  [" << option_idx << "] Go to Battle Phase\n";
                            option_idx++;
                        }

                        if (to_ep) {
                            std::cout << "  [" << option_idx << "] Go to End Phase\n";
                            option_idx++;
                        }

                        // Single choice auto-select
                        if (option_idx == 1) {
                            std::cout << "Only one option, auto-selecting 0\n";
                            pbuf = pbuf_start;
                            int type = -1;
                            count = BufferIO::Read<uint8_t>(pbuf);
                            if (count > 0) type = 0;
                            pbuf += count * 7;
                            count = BufferIO::Read<uint8_t>(pbuf);
                            if (count > 0) type = 1;
                            pbuf += count * 7;
                            count = BufferIO::Read<uint8_t>(pbuf);
                            if (count > 0) type = 2;
                            pbuf += count * 7;
                            count = BufferIO::Read<uint8_t>(pbuf);
                            if (count > 0) type = 3;
                            pbuf += count * 7;
                            count = BufferIO::Read<uint8_t>(pbuf);
                            if (count > 0) type = 4;
                            pbuf += count * 7;
                            count = BufferIO::Read<uint8_t>(pbuf);
                            if (count > 0) type = 5;
                            pbuf += count * 11;
                            if (type < 0 && to_bp) type = 6;
                            else if (type < 0) BufferIO::Read<uint8_t>(pbuf);
                            if (type < 0 && to_ep) type = 7;
                            else if (type < 0) BufferIO::Read<uint8_t>(pbuf);
                            cache_and_set_responsei(pduel, (0 << 16) | type);
                            break;
                        }
                    } else {
                        // Skip all data in auto mode, count options for single-option check
                        pbuf = pbuf_start;
                        int skip_idx = 0;
                        // Summon
                        count = BufferIO::Read<uint8_t>(pbuf);
                        skip_idx += count;
                        for (int i = 0; i < count; ++i) {
                            BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                        }
                        // SP Summon
                        count = BufferIO::Read<uint8_t>(pbuf);
                        skip_idx += count;
                        for (int i = 0; i < count; ++i) {
                            BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                        }
                        // Reposition
                        count = BufferIO::Read<uint8_t>(pbuf);
                        skip_idx += count;
                        for (int i = 0; i < count; ++i) {
                            BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                        }
                        // MSet
                        count = BufferIO::Read<uint8_t>(pbuf);
                        skip_idx += count;
                        for (int i = 0; i < count; ++i) {
                            BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                        }
                        // SSet
                        count = BufferIO::Read<uint8_t>(pbuf);
                        skip_idx += count;
                        for (int i = 0; i < count; ++i) {
                            BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                        }
                        // Activate
                        count = BufferIO::Read<uint8_t>(pbuf);
                        skip_idx += count;
                        for (int i = 0; i < count; ++i) {
                            BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<int32_t>(pbuf);
                        }
                        // Read to_bp, to_ep, shuffle
                        uint8_t a_to_bp = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t a_to_ep = BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        if (a_to_bp) skip_idx++;
                        if (a_to_ep) skip_idx++;
                        // If exactly one option, auto-select it (handled below)
                        option_idx = skip_idx;
                    }

                    if (auto_play) {
                        int chosen = (random_choices && option_idx > 0) ? rand_int_inclusive(0, option_idx - 1) : 0;
                        pbuf = pbuf_start;
                        int current_idx = 0;
                        int type = 0;
                        int index = 0;

                        // Summon
                        count = BufferIO::Read<uint8_t>(pbuf);
                        if (chosen >= current_idx && chosen < current_idx + count) {
                            type = 0;
                            index = chosen - current_idx;
                        } else {
                            current_idx += count;
                            pbuf += count * 7;
                            // SP Summon
                            count = BufferIO::Read<uint8_t>(pbuf);
                            if (chosen >= current_idx && chosen < current_idx + count) {
                                type = 1;
                                index = chosen - current_idx;
                            } else {
                                current_idx += count;
                                pbuf += count * 7;
                                // Reposition
                                count = BufferIO::Read<uint8_t>(pbuf);
                                if (chosen >= current_idx && chosen < current_idx + count) {
                                    type = 2;
                                    index = chosen - current_idx;
                                } else {
                                    current_idx += count;
                                    pbuf += count * 7;
                                    // MSet
                                    count = BufferIO::Read<uint8_t>(pbuf);
                                    if (chosen >= current_idx && chosen < current_idx + count) {
                                        type = 3;
                                        index = chosen - current_idx;
                                    } else {
                                        current_idx += count;
                                        pbuf += count * 7;
                                        // SSet
                                        count = BufferIO::Read<uint8_t>(pbuf);
                                        if (chosen >= current_idx && chosen < current_idx + count) {
                                            type = 4;
                                            index = chosen - current_idx;
                                        } else {
                                            current_idx += count;
                                            pbuf += count * 7;
                                            // Activate
                                            count = BufferIO::Read<uint8_t>(pbuf);
                                            if (chosen >= current_idx && chosen < current_idx + count) {
                                                type = 5;
                                                index = chosen - current_idx;
                                            } else {
                                                current_idx += count;
                                                pbuf += count * 11;
                                                if (to_bp && chosen == current_idx) {
                                                    type = 6;
                                                    index = 0;
                                                } else if (to_ep && chosen == current_idx + (to_bp ? 1 : 0)) {
                                                    type = 7;
                                                    index = 0;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        int response_val = (index << 16) | type;
                        cache_and_set_responsei(pduel, response_val);
                    } else if (option_idx > 1) {
                        std::cout << "\nYour choice (0-" << (option_idx - 1) << "): ";
                        std::cout.flush();
                        std::string line;
                        while (true) {
                            std::getline(std::cin, line);
                            if (line.empty()) continue;
                            try {
                                int choice = std::stoi(line);
                                if (choice >= 0 && choice < option_idx) {
                                    // Re-parse from start to determine type and index
                                    pbuf = pbuf_start;
                                    int current_idx = 0;
                                    int type = 0;
                                    int index = 0;

                                    // Summon
                                    count = BufferIO::Read<uint8_t>(pbuf);
                                    if (choice >= current_idx && choice < current_idx + count) {
                                        type = 0;
                                        index = choice - current_idx;
                                    } else {
                                        current_idx += count;
                                        pbuf += count * 7;
                                        // SP Summon
                                        count = BufferIO::Read<uint8_t>(pbuf);
                                        if (choice >= current_idx && choice < current_idx + count) {
                                            type = 1;
                                            index = choice - current_idx;
                                        } else {
                                            current_idx += count;
                                            pbuf += count * 7;
                                            // Reposition
                                            count = BufferIO::Read<uint8_t>(pbuf);
                                            if (choice >= current_idx && choice < current_idx + count) {
                                                type = 2;
                                                index = choice - current_idx;
                                            } else {
                                                current_idx += count;
                                                pbuf += count * 7;
                                                // MSet
                                                count = BufferIO::Read<uint8_t>(pbuf);
                                                if (choice >= current_idx && choice < current_idx + count) {
                                                    type = 3;
                                                    index = choice - current_idx;
                                                } else {
                                                    current_idx += count;
                                                    pbuf += count * 7;
                                                    // SSet
                                                    count = BufferIO::Read<uint8_t>(pbuf);
                                                    if (choice >= current_idx && choice < current_idx + count) {
                                                        type = 4;
                                                        index = choice - current_idx;
                                                    } else {
                                                        current_idx += count;
                                                        pbuf += count * 7;
                                                        // Activate
                                                        count = BufferIO::Read<uint8_t>(pbuf);
                                                        if (choice >= current_idx && choice < current_idx + count) {
                                                            type = 5;
                                                            index = choice - current_idx;
                                                        } else {
                                                            current_idx += count;
                                                            pbuf += count * 11;
                                                            if (to_bp && choice == current_idx) {
                                                                type = 6;
                                                                index = 0;
                                                            } else if (to_ep && choice == current_idx + (to_bp ? 1 : 0)) {
                                                                type = 7;
                                                                index = 0;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    int response_val = (index << 16) | type;
                                    cache_and_set_responsei(pduel, response_val);
                                    break;
                                }
                            } catch (...) {}
                            std::cout << "Invalid choice, try again: ";
                            std::cout.flush();
                        }
                    }

                    // Restore pbuf to end of message
                    pbuf = pbuf_end;
                    break;
                }
                case MSG_SELECT_BATTLECMD: {
                    display_state_before = true;
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    (void)player;
                    std::cout << "Player " << (int)player << " battle command\n";

                   uint8_t* pbuf_start = pbuf;
                    uint8_t count;
                    int total_options = 0;

                    // First count options
                    // Activatable: each is 11 bytes {code:int32, ctrl:u8, loc:u8, seq:u8, desc:int32}
                    count = BufferIO::Read<uint8_t>(pbuf);
                    total_options += count;
                    for (int i = 0; i < count; ++i) {
                        uint32_t code = BufferIO::Read<int32_t>(pbuf);  // code
                        uint8_t ctrl = BufferIO::Read<uint8_t>(pbuf);  // controler
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);  // location
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);  // sequence
                        int32_t desc = BufferIO::Read<int32_t>(pbuf);  // description
                    }

                    // Attackable: each is 8 bytes {code:int32, ctrl:u8, loc:u8, seq:u8, direct_attackable:u8}
                    count = BufferIO::Read<uint8_t>(pbuf);
                    total_options += count;
                    for (int i = 0; i < count; ++i) {
                        uint32_t code = BufferIO::Read<int32_t>(pbuf);  // code
                        uint8_t ctrl = BufferIO::Read<uint8_t>(pbuf);  // controler
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);  // location
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);  // sequence
                        uint8_t diratt = BufferIO::Read<uint8_t>(pbuf);  // direct_attackable
                    }

                    uint8_t to_m2 = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t to_ep = BufferIO::Read<uint8_t>(pbuf);
                    if (to_m2) total_options++;
                    if (to_ep) total_options++;

                    // SAVE END POSITION
                    uint8_t* pbuf_end = pbuf;

                    std::cout << "Total options: " << total_options << "\n";

                    if (!auto_play) {
                        display_game_state();
                    }

                    pbuf = pbuf_start;
                    int option_idx = 0;

                    if (!auto_play) {
                        // Activatable
                        count = BufferIO::Read<uint8_t>(pbuf);
                        for (int i = 0; i < count; ++i) {
                            uint32_t code = BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            int32_t desc = BufferIO::Read<int32_t>(pbuf);
                            bool is_operation = (code & 0x80000000) != 0;
                            if (is_operation) {
                                code &= 0x7fffffff;
                            }
                            if (is_operation && code > 0) {
                                std::cout << "  [" << option_idx << "] " << get_card_name(code) << "\n";
                            } else if (is_operation) {
                                std::cout << "  [" << option_idx << "] Operation " << desc << "\n";
                            } else if (code > 0) {
                                std::cout << "  [" << option_idx << "] Activate " << get_card_name(code) << "\n";
                            } else {
                                std::cout << "  [" << option_idx << "] Option\n";
                            }
                            option_idx++;
                        }

                        // Attackable
                        count = BufferIO::Read<uint8_t>(pbuf);
                        for (int i = 0; i < count; ++i) {
                            uint32_t code = BufferIO::Read<int32_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            BufferIO::Read<uint8_t>(pbuf);
                            std::cout << "  [" << option_idx << "] Attack with " << get_card_name(code) << "\n";
                            option_idx++;
                        }

                        if (to_m2) {
                            std::cout << "  [" << option_idx << "] Go to Main Phase 2\n";
                            option_idx++;
                        }

                        if (to_ep) {
                            std::cout << "  [" << option_idx << "] Go to End Phase\n";
                            option_idx++;
                        }
                    } else {
                        // Skip activatable data (11 bytes each: 4+1+1+1+4)
                        count = BufferIO::Read<uint8_t>(pbuf);
                        pbuf += count * 11;
                        // Skip attackable data (8 bytes each: 4+1+1+1+1)
                        count = BufferIO::Read<uint8_t>(pbuf);
                        pbuf += count * 8;
                    }

                    if (total_options == 1) {
                        std::cout << "Only one option, auto-selecting\n";
                        pbuf = pbuf_start;
                        int type = -1;
                        // Activatable
                        count = BufferIO::Read<uint8_t>(pbuf);
                        if (count > 0) type = 0;
                        pbuf += count * 11;
                        // Attackable
                        if (type < 0) {
                            count = BufferIO::Read<uint8_t>(pbuf);
                            if (count > 0) type = 1;
                            pbuf += count * 8;
                        }
                        if (type < 0 && to_m2) type = 2;
                        else if (type < 0) BufferIO::Read<uint8_t>(pbuf);
                        if (type < 0 && to_ep) type = 3;
                        else if (type < 0) BufferIO::Read<uint8_t>(pbuf);
                        int response_val = (0 << 16) | type;
                        cache_and_set_responsei(pduel, response_val);
                    } else if (auto_play) {
                        int chosen = (random_choices && total_options > 0) ? rand_int_inclusive(0, total_options - 1) : 0;
                        pbuf = pbuf_start;
                        int current_idx = 0;
                        int type = 0;
                        int index = 0;
                        // Activatable
                        count = BufferIO::Read<uint8_t>(pbuf);
                        if (chosen >= current_idx && chosen < current_idx + count) {
                            type = 0;
                            index = chosen - current_idx;
                        } else {
                            current_idx += count;
                            pbuf += count * 11;
                            // Attackable
                            count = BufferIO::Read<uint8_t>(pbuf);
                            if (chosen >= current_idx && chosen < current_idx + count) {
                                type = 1;
                                index = chosen - current_idx;
                            } else {
                                current_idx += count;
                                pbuf += count * 8;
                                if (to_m2 && chosen == current_idx) {
                                    type = 2;
                                    index = 0;
                                } else if (to_ep && chosen == current_idx + (to_m2 ? 1 : 0)) {
                                    type = 3;
                                    index = 0;
                                }
                            }
                        }
                        int response_val = (index << 16) | type;
                        cache_and_set_responsei(pduel, response_val);
                    } else if (total_options > 1) {
                        std::cout << "\nYour choice (0-" << (total_options - 1) << "): ";
                        std::cout.flush();
                        std::string line;
                        while (true) {
                            std::getline(std::cin, line);
                            if (line.empty()) continue;
                            try {
                                int choice = std::stoi(line);
                                if (choice >= 0 && choice < total_options) {
                                    // Re-parse from start to determine type and index
                                    pbuf = pbuf_start;
                                    int current_idx = 0;
                                    int type = 0;
                                    int index = 0;

                                    // Activatable
                                    count = BufferIO::Read<uint8_t>(pbuf);
                                    if (choice >= current_idx && choice < current_idx + count) {
                                        type = 0;
                                        index = choice - current_idx;
                                    } else {
                                        current_idx += count;
                                        pbuf += count * 11;
                                        // Attackable
                                        count = BufferIO::Read<uint8_t>(pbuf);
                                        if (choice >= current_idx && choice < current_idx + count) {
                                            type = 1;
                                            index = choice - current_idx;
                                        } else {
                                            current_idx += count;
                                            pbuf += count * 8;
                                            if (to_m2 && choice == current_idx) {
                                                type = 2;
                                                index = 0;
                                            } else if (to_ep && choice == current_idx + (to_m2 ? 1 : 0)) {
                                                type = 3;
                                                index = 0;
                                            }
                                        }
                                    }
                                    int response_val = (index << 16) | type;
                                    cache_and_set_responsei(pduel, response_val);
                                    break;
                                }
                            } catch (...) {}
                            std::cout << "Invalid choice, try again (0-" << (total_options - 1) << "): ";
                            std::cout.flush();
                        }
                    }
                    // Restore pbuf to end of message
                    pbuf = pbuf_end;
                    break;
                }
                case MSG_SELECT_YESNO:
                case MSG_SELECT_EFFECTYN: {
                    display_state_before = true;
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint32_t code = 0;
                    uint32_t desc = 0;
                    if (msg_type == MSG_SELECT_EFFECTYN) {
                        code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t ctrl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        (void)ctrl; (void)loc; (void)seq;
                        desc = BufferIO::Read<uint32_t>(pbuf);
                        std::cout << "Player " << (int)player << ": Activate effect of " << get_card_name(code) << "? (desc=" << desc << ")\n";
                    } else {
                        desc = BufferIO::Read<uint32_t>(pbuf);
                        std::cout << "Player " << (int)player << ": Yes/No? (desc=" << desc << ")\n";
                    }
                    if (!auto_play) {
                        display_game_state();
                    }
                    if (auto_play) {
                        cache_and_set_responsei(pduel, random_choices ? rand_int_inclusive(0, 1) : 0);
                    } else {
                        std::cout << "0: No\n1: Yes\nYour choice: ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        try { int choice = std::stoi(line); cache_and_set_responsei(pduel, choice); }
                        catch (...) { cache_and_set_responsei(pduel, 0); }
                    }
                    break;
                }
                case MSG_SELECT_OPTION: {
                    display_state_before = true;
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " select option (" << (int)count << " options):\n";
                    if (!auto_play) {
                        display_game_state();
                    }
                    for(int i = 0; i < count; i++) {
                        int32_t opt = BufferIO::Read<int32_t>(pbuf);
                        if (!auto_play) {
                            std::cout << "  [" << i << "] Option " << opt << "\n";
                        }
                    }
                    if (count == 1) {
                        std::cout << "Only one option, auto-selecting 0\n";
                        cache_and_set_responsei(pduel, 0);
                    } else if (auto_play) {
                        cache_and_set_responsei(pduel, count > 0 ? (random_choices ? rand_int_inclusive(0, count - 1) : 0) : -1);
                    } else {
                        std::cout << "Your choice (0-" << (int)(count-1) << "): ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        try { int choice = std::stoi(line); cache_and_set_responsei(pduel, choice); }
                        catch (...) { cache_and_set_responsei(pduel, 0); }
                    }
                    break;
                }
                case MSG_SELECT_CARD:
                case MSG_SELECT_TRIBUTE:
                case MSG_SELECT_UNSELECT_CARD: {
                    display_state_before = true;
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t cancelable = 0;
                    uint8_t min = 0;
                    uint8_t max = 0;
                    if (msg_type == MSG_SELECT_UNSELECT_CARD) {
                        uint8_t finishable = BufferIO::Read<uint8_t>(pbuf);
                        cancelable = BufferIO::Read<uint8_t>(pbuf);
                        (void)finishable;
                    } else {
                        cancelable = BufferIO::Read<uint8_t>(pbuf);
                    }
                    min = BufferIO::Read<uint8_t>(pbuf);
                    max = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " select card"
                              << " (cancelable=" << (int)cancelable
                              << ", min=" << (int)min
                              << ", max=" << (int)max
                              << ", count=" << (int)count << "):\n";
                    if (!auto_play) {
                        display_game_state();
                    }
                    for(int i = 0; i < count; i++) {
                        uint32_t code = BufferIO::Read<int32_t>(pbuf);
                        uint8_t ctrl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t sub_seq = BufferIO::Read<uint8_t>(pbuf);
                        (void)ctrl; (void)loc; (void)seq; (void)sub_seq;
                        if (!auto_play) {
                            std::cout << "  [" << i << "] " << get_card_name(code) << "\n";
                        }
                    }
                    // Read additional cards for MSG_SELECT_UNSELECT_CARD
                    if (msg_type == MSG_SELECT_UNSELECT_CARD) {
                        uint8_t selected_count = BufferIO::Read<uint8_t>(pbuf);
                        std::cout << "Selected cards (" << (int)selected_count << "):\n";
                        for(int i = 0; i < selected_count; i++) {
                            uint32_t code = BufferIO::Read<int32_t>(pbuf);
                            uint8_t ctrl = BufferIO::Read<uint8_t>(pbuf);
                            uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                            uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                            uint8_t sub_seq = BufferIO::Read<uint8_t>(pbuf);
                            (void)ctrl; (void)loc; (void)seq; (void)sub_seq;
                            if (!auto_play) {
                                std::cout << "  [+" << i << "] " << get_card_name(code) << "\n";
                            }
                        }
                    }
                    if (count == (int)min && !cancelable && min == max) {
                        std::cout << "Auto-selecting (no choice)\n";
                        if (msg_type == MSG_SELECT_UNSELECT_CARD) {
                            uint8_t buf[2] = {1, 0};
                            cache_and_set_responseb(pduel, buf, 2);
                        } else {
                            std::vector<uint8_t> indices;
                            for(int i = 0; i < min; i++) {
                                indices.push_back(static_cast<uint8_t>(i));
                            }
                            send_selected_indices_response(pduel, indices);
                        }
                    } else if (auto_play) {
                        if (msg_type == MSG_SELECT_UNSELECT_CARD) {
                            if (count > 0) {
                                uint8_t idx = static_cast<uint8_t>(random_choices ? rand_int_inclusive(0, count - 1) : 0);
                                uint8_t buf[2] = {1, idx};
                                cache_and_set_responseb(pduel, buf, 2);
                            } else {
                                uint8_t buf[2] = {0, 0};
                                cache_and_set_responseb(pduel, buf, 2);
                            }
                        } else {
                            int choose_n = min;
                            if (random_choices) {
                                choose_n = rand_int_inclusive(min, max);
                            }
                            if (choose_n > count) choose_n = count;
                            std::vector<uint8_t> indices;
                            if (random_choices) {
                                indices = random_unique_indices(count, choose_n);
                            } else {
                                for (int i = 0; i < min && i < count; ++i) {
                                    indices.push_back(static_cast<uint8_t>(i));
                                }
                            }
                            send_selected_indices_response(pduel, indices);
                        }
  } else {
                        if (msg_type == MSG_SELECT_UNSELECT_CARD) {
                            std::cout << "Your choice (index, -1 to cancel): ";
                            std::cout.flush();
                            std::string line;
                            std::getline(std::cin, line);
                            try {
                                int choice = std::stoi(line);
                                if (choice == -1) {
                                    // Send all 0xFF for cancel (engine checks ivalue[0] == -1)
                                    uint8_t buf[4] = {0xff, 0xff, 0xff, 0xff};
                                    cache_and_set_responseb(pduel, buf, 4);
                                } else {
                                    uint8_t buf[2] = {1, (uint8_t)choice};
                                    cache_and_set_responseb(pduel, buf, 2);
                                }
                            } catch (...) {
                                uint8_t buf[2] = {1, 0};
                                cache_and_set_responseb(pduel, buf, 2);
                            }
                        } else {
                            std::cout << "Your choice (" << (cancelable ? "space/comma-separated indices, -1 to cancel" : "space/comma-separated indices") << "): ";
                            std::cout.flush();
                            std::string line;
                            std::getline(std::cin, line);
                            if (cancelable) {
                                try {
                                    int cancel_choice = std::stoi(line);
                                    if (cancel_choice == -1) {
                                        cache_and_set_responsei(pduel, -1);
                                        break;
                                    }
                                } catch (...) {}
                            }
                            std::replace(line.begin(), line.end(), ',', ' ');
                            std::stringstream ss(line);
                            std::vector<uint8_t> indices;
                            int idx = -1;
                            while(ss >> idx) {
                                if(idx >= 0 && idx < count) {
                                    indices.push_back(static_cast<uint8_t>(idx));
                                }
                            }
                            if(indices.size() < min) {
                                std::cout << "Invalid choice, try again\n";
                                continue;
                            }
                            if(indices.size() > max) {
                                indices.resize(max);
                            }
                            send_selected_indices_response(pduel, indices);
                        }
                    }
                    break;
                }
                case MSG_SELECT_CHAIN: {
                    display_state_before = true;
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t chain_count = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t spe_count = BufferIO::Read<uint8_t>(pbuf);
                    uint32_t hint0 = BufferIO::Read<uint32_t>(pbuf);
                    uint32_t hint1 = BufferIO::Read<uint32_t>(pbuf);
                    (void)spe_count; (void)hint0; (void)hint1;
                    std::cout << "Player " << (int)player << " chain (count=" << (int)chain_count
                              << ", spe_count=" << (int)spe_count << "):\n";
                    if (!auto_play) {
                        display_game_state();
                    }
                    bool any_forced_chain = false;
                    for(int i = 0; i < chain_count; i++) {
                        uint8_t flag = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t forced_chain = BufferIO::Read<uint8_t>(pbuf);
                        if (forced_chain) {
                            any_forced_chain = true;
                        }
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint32_t info_location = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t ctrl = info_location >> 24;
                        uint8_t loc = (info_location >> 16) & 0xFF;
                        uint8_t seq = (info_location >> 8) & 0xFF;
                        uint32_t desc = BufferIO::Read<uint32_t>(pbuf);
                        (void)ctrl; (void)loc; (void)seq;
                        if (!auto_play) {
                            std::cout << "  [" << i << "] " << get_card_name(code)
                                      << " (flag=" << (int)flag
                                      << ", forced=" << (int)forced_chain
                                      << ", desc=" << desc << ")\n";
                        }
                    }
                    if (chain_count == 0 && !spe_count) {
                        std::cout << "No chain options, auto-selecting -1\n";
                        cache_and_set_responsei(pduel, -1);
                    } else if (chain_count == 1 && !spe_count) {
                        std::cout << "Only one chain option, auto-selecting 0\n";
                        cache_and_set_responsei(pduel, 0);
                    } else if (auto_play) {
                        bool can_pass = !any_forced_chain && !spe_count;
                        if (random_choices && chain_count > 0) {
                            if (can_pass) {
                                cache_and_set_responsei(pduel, rand_int_inclusive(-1, chain_count - 1));
                            } else {
                                cache_and_set_responsei(pduel, rand_int_inclusive(0, chain_count - 1));
                            }
                        } else {
                            cache_and_set_responsei(pduel, can_pass ? -1 : 0);
                        }
                    } else {
                        std::cout << "Your choice (-1 to not chain, 0-" << (int)(chain_count-1) << "): ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        try { int choice = std::stoi(line); cache_and_set_responsei(pduel, choice); }
                        catch (...) { cache_and_set_responsei(pduel, -1); }
                    }
                    break;
                }
                case MSG_SELECT_PLACE:
                case MSG_SELECT_DISFIELD: {
                    display_state_before = true;
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    uint32_t forbidden = BufferIO::Read<uint32_t>(pbuf);
                    uint32_t selectable = ~forbidden;

                    std::cout << "Player " << (int)player << " select place"
                              << " (count=" << (int)count
                              << ", forbidden=0x" << std::hex << forbidden << std::dec << ")\n";
                    if (!auto_play) {
                        display_game_state();
                    }

                    struct PlaceOption {
                        int ctrl;
                        int loc;
                        int seq;
                        std::string name;
                    };
                    std::vector<PlaceOption> options;

                    for (int i = 0; i < 7; i++) {
                        if (selectable & (0x1U << i)) {
                            options.push_back({0, LOCATION_MZONE, i, "P0-MZone[" + std::to_string(i) + "]"});
                        }
                    }
                    for (int i = 0; i < 8; i++) {
                        if (selectable & (0x100U << i)) {
                            options.push_back({0, LOCATION_SZONE, i, "P0-SZone[" + std::to_string(i) + "]"});
                        }
                    }
                    for (int i = 0; i < 7; i++) {
                        if (selectable & (0x10000U << i)) {
                            options.push_back({1, LOCATION_MZONE, i, "P1-MZone[" + std::to_string(i) + "]"});
                        }
                    }
                    for (int i = 0; i < 8; i++) {
                        if (selectable & (0x1000000U << i)) {
                            options.push_back({1, LOCATION_SZONE, i, "P1-SZone[" + std::to_string(i) + "]"});
                        }
                    }

                    if (!auto_play) {
                        std::cout << "Available options:\n";
                        for (size_t i = 0; i < options.size(); i++) {
                            std::cout << "  [" << i << "] " << options[i].name << "\n";
                        }
                    }

                    unsigned char buf[3] = {0, LOCATION_MZONE, 0};
                    int selected_idx = 0;
                    if (auto_play && random_choices && !options.empty()) {
                        selected_idx = rand_int_inclusive(0, static_cast<int>(options.size()) - 1);
                    }

                    if (options.size() > 0) {
                        buf[0] = options[selected_idx].ctrl;
                        buf[1] = options[selected_idx].loc;
                        buf[2] = options[selected_idx].seq;
                    }

                    if (auto_play || options.size() == 1) {
                        if (options.size() == 1 && !auto_play) {
                            std::cout << "Only one option, auto-selecting " << options[0].name << "\n";
                        }
                        cache_and_set_responseb(pduel, buf, 3);
                    } else if (options.size() > 1) {
                        std::cout << "\nYour choice (0-" << (options.size() - 1) << "): ";
                        std::cout.flush();
                        std::string line;
                        while (true) {
                            std::getline(std::cin, line);
                            if (line.empty()) continue;
                            try {
                                int choice = std::stoi(line);
                                if (choice >= 0 && choice < (int)options.size()) {
                                    selected_idx = choice;
                                    buf[0] = options[selected_idx].ctrl;
                                    buf[1] = options[selected_idx].loc;
                                    buf[2] = options[selected_idx].seq;
                                    cache_and_set_responseb(pduel, buf, 3);
                                    break;
                                }
                            } catch (...) {}
                            std::cout << "Invalid choice, try again: ";
                            std::cout.flush();
                        }
                    } else {
                        cache_and_set_responseb(pduel, buf, 3);
                    }
                    break;
                }
                case MSG_SELECT_POSITION: {
                    display_state_before = true;
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                    uint8_t positions = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " select position for " << get_card_name(code) << ":\n";
                    if (!auto_play) {
                        display_game_state();
                    }
                    int pos_count = 0;
                    if (!auto_play) {
                        int opt_idx = 0;
                        if (positions & POS_FACEUP_ATTACK) {
                            std::cout << "  [" << opt_idx++ << "] Face-Up Attack\n";
                            pos_count++;
                        }
                        if (positions & POS_FACEDOWN_ATTACK) {
                            std::cout << "  [" << opt_idx++ << "] Face-Down Attack\n";
                            pos_count++;
                        }
                        if (positions & POS_FACEUP_DEFENSE) {
                            std::cout << "  [" << opt_idx++ << "] Face-Up Defense\n";
                            pos_count++;
                        }
                        if (positions & POS_FACEDOWN_DEFENSE) {
                            std::cout << "  [" << opt_idx++ << "] Face-Down Defense\n";
                            pos_count++;
                        }
                    }
                    int pos = POS_FACEUP_ATTACK;
                    std::vector<int> valid_positions;
                    if (positions & POS_FACEUP_ATTACK) valid_positions.push_back(POS_FACEUP_ATTACK);
                    if (positions & POS_FACEDOWN_ATTACK) valid_positions.push_back(POS_FACEDOWN_ATTACK);
                    if (positions & POS_FACEUP_DEFENSE) valid_positions.push_back(POS_FACEUP_DEFENSE);
                    if (positions & POS_FACEDOWN_DEFENSE) valid_positions.push_back(POS_FACEDOWN_DEFENSE);
                    if (!valid_positions.empty()) {
                        if (random_choices && auto_play) {
                            pos = valid_positions[rand_int_inclusive(0, static_cast<int>(valid_positions.size()) - 1)];
                        } else {
                            pos = valid_positions[0];
                        }
                    }
                    if (auto_play || pos_count == 1) {
                        cache_and_set_responsei(pduel, pos);
                    } else {
                        std::cout << "Your choice (0-" << ((int)valid_positions.size() - 1) << "): ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        try {
                            int choice = std::stoi(line);
                            if (choice >= 0 && choice < (int)valid_positions.size())
                                pos = valid_positions[choice];
                        } catch (...) {}
                        cache_and_set_responsei(pduel, pos);
                    }
                    break;
                }
                case MSG_SELECT_SUM: {
                    display_state_before = true;
                    uint8_t select_mode = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint32_t sumval = BufferIO::Read<uint32_t>(pbuf);
                    uint8_t min = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t max = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t must_count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " select summon material"
                              << " (mode=" << (int)select_mode
                              << ", sumval=" << sumval
                              << ", min=" << (int)min
                              << ", max=" << (int)max
                              << ", must=" << (int)must_count << "):\n";
                    if (!auto_play) {
                        display_game_state();
                    }
                    for(int i = 0; i < must_count; i++) {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t ctrl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t param = BufferIO::Read<uint32_t>(pbuf);
                        (void)ctrl; (void)loc; (void)seq; (void)param;
                        if (!auto_play) {
                            std::cout << "  [MUST] " << get_card_name(code) << "\n";
                        }
                    }
                    uint8_t select_count = BufferIO::Read<uint8_t>(pbuf);
                    for(int i = 0; i < select_count; i++) {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t ctrl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint32_t param = BufferIO::Read<uint32_t>(pbuf);
                        (void)ctrl; (void)loc; (void)seq; (void)param;
                        if (!auto_play) {
                            std::cout << "  [" << i << "] " << get_card_name(code) << "\n";
                        }
                    }
                    if (select_count == 0 && min == 0) {
                        std::cout << "No choices needed, auto-selecting\n";
                        unsigned char buf[512] = {0};
                        int total = must_count;
                        buf[0] = total;
                        buf[1] = 0;
                        cache_and_set_responseb(pduel, buf, 2);
                    } else if (auto_play) {
                        if (select_count > 0) {
                            unsigned char buf[512] = {0};
                            int extra_pick = (min > 0 ? min : 1);
                            if (random_choices) {
                                extra_pick = rand_int_inclusive(min, max);
                                if (extra_pick <= 0) extra_pick = 1;
                            }
                            int total = must_count + extra_pick;
                            if (total > must_count + select_count) total = must_count + select_count;
                            buf[0] = total;
                            buf[1] = 0;
                            cache_and_set_responseb(pduel, buf, 2);
                        } else {
                            cache_and_set_responsei(pduel, -1);
                        }
                    } else {
                        std::cout << "Your choice (-1 to cancel, optional card indices 0-" << (int)(select_count-1) << "): ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        std::istringstream iss(line);
                        std::vector<int> picks;
                        int v;
                        while (iss >> v) picks.push_back(v);
                        if (picks.empty() || picks[0] < 0) {
                            cache_and_set_responsei(pduel, -1);
                        } else {
                            // Response: count, then must-card indices, then chosen optional indices
                            std::vector<uint8_t> buf;
                            buf.push_back((uint8_t)(must_count + picks.size()));
                            for (int i = 0; i < must_count; i++) buf.push_back((uint8_t)i);
                            for (int p : picks)
                                if (p >= 0 && p < (int)select_count) buf.push_back((uint8_t)p);
                            cache_and_set_responseb(pduel, buf.data(), (int)buf.size());
                        }
                    }
                    break;
                }
                case MSG_SELECT_COUNTER: {
                    display_state_before = true;
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint16_t counter_type = BufferIO::Read<uint16_t>(pbuf);
                    uint16_t counter_count = BufferIO::Read<uint16_t>(pbuf);
                    uint8_t card_count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " select counter"
                              << " (type=" << counter_type
                              << ", count=" << counter_count
                              << ", cards=" << (int)card_count << "):\n";
                    if (!auto_play) {
                        display_game_state();
                    }
                    for(int i = 0; i < card_count; i++) {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t ctrl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        uint16_t cnt = BufferIO::Read<uint16_t>(pbuf);
                        (void)ctrl; (void)loc; (void)seq; (void)cnt;
                        if (!auto_play) {
                            std::cout << "  [" << i << "] " << get_card_name(code) << "\n";
                        }
                    }
                    if (auto_play) {
                        cache_and_set_responsei(pduel, random_choices && card_count > 0 ? rand_int_inclusive(0, card_count - 1) : 0);
                    } else {
                        std::cout << "Your choice: ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        cache_and_set_responsei(pduel, 0);
                    }
                    break;
                }
                case MSG_ROCK_PAPER_SCISSORS: {
                    display_state_before = true;
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    (void)player;
                    std::cout << "Rock-Paper-Scissors\n";
                    display_game_state();
                    if (auto_play) {
                        cache_and_set_responsei(pduel, 0);
                    } else {
                        std::cout << "0: Rock\n1: Paper\n2: Scissors\nYour choice: ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        try {
                            int choice = std::stoi(line);
                            if (choice >=0 && choice <= 2) cache_and_set_responsei(pduel, choice);
                            else cache_and_set_responsei(pduel, 0);
                        } catch (...) { cache_and_set_responsei(pduel, 0); }
                    }
                    break;
                }
                case MSG_SHUFFLE_HAND: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " shuffles hand (" << (int)count << " cards)\n";
                    for(int i = 0; i < count; i++) {
                        BufferIO::Read<uint32_t>(pbuf);
                    }
                    break;
                }
                case MSG_SHUFFLE_EXTRA: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " shuffles extra (" << (int)count << " cards)\n";
                    for(int i = 0; i < count; i++) {
                        BufferIO::Read<uint32_t>(pbuf);
                    }
                    break;
                }
                case MSG_CONFIRM_DECKTOP:
                case MSG_CONFIRM_EXTRATOP: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " confirms deck top (" << (int)count << " cards): ";
                    for(int i = 0; i < count; i++) {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        if (i > 0) std::cout << ", ";
                        std::cout << get_card_name(code);
                    }
                    std::cout << "\n";
                    break;
                }
                case MSG_CONFIRM_CARDS: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t skip = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    (void)skip;
                    std::cout << "Player " << (int)player << " confirms " << (int)count << " cards: ";
                    for(int i = 0; i < count; i++) {
                        uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                        uint8_t ctrl = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                        uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                        (void)ctrl; (void)loc; (void)seq;
                        if (i > 0) std::cout << ", ";
                        std::cout << get_card_name(code);
                    }
                    std::cout << "\n";
                    break;
                }
                case MSG_TOSS_COIN: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " tosses " << (int)count << " coins: ";
                    for(int i = 0; i < count; i++) {
                        uint8_t res = BufferIO::Read<uint8_t>(pbuf);
                        if (i > 0) std::cout << ", ";
                        std::cout << (res ? "Heads" : "Tails");
                    }
                    std::cout << "\n";
                    break;
                }
                case MSG_TOSS_DICE: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " rolls " << (int)count << " dice: ";
                    for(int i = 0; i < count; i++) {
                        uint8_t res = BufferIO::Read<uint8_t>(pbuf);
                        if (i > 0) std::cout << ", ";
                        std::cout << (int)res;
                    }
                    std::cout << "\n";
                    break;
                }
                case MSG_ANNOUNCE_RACE:
                case MSG_ANNOUNCE_ATTRIB:
                case MSG_ANNOUNCE_CARD:
                case MSG_ANNOUNCE_NUMBER: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " announces: ";
                    if (msg_type == MSG_ANNOUNCE_RACE || msg_type == MSG_ANNOUNCE_ATTRIB) {
                        uint32_t val = BufferIO::Read<uint32_t>(pbuf);
                        std::cout << val;
                    } else if (msg_type == MSG_ANNOUNCE_CARD || msg_type == MSG_ANNOUNCE_NUMBER) {
                        uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                        for(int i = 0; i < count; i++) {
                            uint32_t val = BufferIO::Read<uint32_t>(pbuf);
                            if (i > 0) std::cout << ", ";
                            std::cout << val;
                        }
                    }
                    std::cout << "\n";
                    if (auto_play) {
                        cache_and_set_responsei(pduel, 0);
                    } else if (msg_type == MSG_ANNOUNCE_RACE || msg_type == MSG_ANNOUNCE_ATTRIB ||
                               msg_type == MSG_ANNOUNCE_CARD || msg_type == MSG_ANNOUNCE_NUMBER) {
                        std::cout << "Your choice: ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        int v = 0;
                        try { v = std::stoi(line); } catch (...) {}
                        cache_and_set_responsei(pduel, v);
                    }
                    break;
                }
                case MSG_ADD_COUNTER: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t location = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t sequence = BufferIO::Read<uint8_t>(pbuf);
                    uint16_t counter_type = BufferIO::Read<uint16_t>(pbuf);
                    uint16_t count = BufferIO::Read<uint16_t>(pbuf);
                    std::cout << "Player " << (int)player << " adds " << count
                              << " counter(type=" << counter_type
                              << ") to " << location_name(location) << "[" << (int)sequence << "]\n";
                    break;
                }
                case MSG_REMOVE_COUNTER: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t location = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t sequence = BufferIO::Read<uint8_t>(pbuf);
                    uint16_t counter_type = BufferIO::Read<uint16_t>(pbuf);
                    uint16_t count = BufferIO::Read<uint16_t>(pbuf);
                    std::cout << "Player " << (int)player << " removes " << count
                              << " counter(type=" << counter_type
                              << ") from " << location_name(location) << "[" << (int)sequence << "]\n";
                    break;
                }
                case MSG_EQUIP: {
                    uint8_t eq_ctrl = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t eq_loc = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t eq_seq = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t tgt_ctrl = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t tgt_loc = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t tgt_seq = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Equip: " << location_name(eq_loc) << "[" << (int)eq_seq << "] (player " << (int)eq_ctrl << ")"
                              << " -> " << location_name(tgt_loc) << "[" << (int)tgt_seq << "] (player " << (int)tgt_ctrl << ")\n";
                    needs_display_after = true;
                    break;
                }
                case MSG_CARD_TARGET: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t location = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t sequence = BufferIO::Read<uint8_t>(pbuf);
                    uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                    std::cout << "Card target: " << get_card_name(code)
                              << " at " << location_name(location) << "[" << (int)sequence << "] (player " << (int)player << ")\n";
                    break;
                }
                case MSG_CANCEL_TARGET: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t location = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t sequence = BufferIO::Read<uint8_t>(pbuf);
                    uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                    std::cout << "Cancel target: " << get_card_name(code)
                              << " at " << location_name(location) << "[" << (int)sequence << "] (player " << (int)player << ")\n";
                    break;
                }
                case MSG_BECOME_TARGET: {
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Become target: " << (int)count << " cards\n";
                    for(int i = 0; i < count; i++) {
                        uint32_t code = BufferIO::Read<int32_t>(pbuf);
                        std::cout << "  " << get_card_name(code) << "\n";
                    }
                    break;
                }
                case MSG_RANDOM_SELECTED: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Selected: Player " << (int)player << " " << (int)count << " cards: ";
                    for(int i = 0; i < count; i++) {
                        uint32_t code = BufferIO::Read<int32_t>(pbuf);
                        if (i > 0) std::cout << ", ";
                        std::cout << get_card_name(code);
                    }
                    std::cout << "\n";
                    break;
                }
               case MSG_SET: {
                    uint32_t code = BufferIO::Read<uint32_t>(pbuf);
                    uint8_t ctrl = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t seq = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t pos = BufferIO::Read<uint8_t>(pbuf);
                    (void)pos;
                    std::cout << "Set: " << get_card_name(code)
                              << " to " << location_name(loc) << "[" << (int)seq << "] (player " << (int)ctrl << ")\n";
                    needs_display_after = true;
                    break;
                }
                case MSG_SWAP: {
                    uint32_t code1 = BufferIO::Read<uint32_t>(pbuf);
                    uint8_t p1 = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t l1 = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t s1 = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t pos1 = BufferIO::Read<uint8_t>(pbuf);
                    uint32_t code2 = BufferIO::Read<uint32_t>(pbuf);
                    uint8_t p2 = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t l2 = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t s2 = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t pos2 = BufferIO::Read<uint8_t>(pbuf);
                    (void)code1;
                    (void)code2;
                    (void)pos1;
                    (void)pos2;
                    std::cout << "Swap: " << location_name(l1) << "[" << (int)s1 << "] (p" << (int)p1 << ")"
                              << " <-> " << location_name(l2) << "[" << (int)s2 << "] (p" << (int)p2 << ")\n";
                    needs_display_after = true;
                    break;
                }
                case MSG_CHAIN_END: {
                    std::cout << "Chain end\n";
                    needs_display_after = true;
                    break;
                }
                case MSG_CHAIN_SOLVED: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Chain solved (player " << (int)player << ")\n";
                    break;
                }
                case MSG_SORT_CARD: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Sort card: Player " << (int)player << ", " << (int)count << " cards\n";
                    for(int i = 0; i < count; i++) {
                        BufferIO::Read<int32_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                        BufferIO::Read<uint8_t>(pbuf);
                    }
                    if (auto_play) {
                        uint32_t resp = 0;
                        cache_and_set_responsei(pduel, resp);
                    } else {
                        std::cout << "Your choice (enter " << (int)count
                                  << " card indices 0-" << (int)(count-1)
                                  << " in desired order, or 'keep'): ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        std::istringstream iss(line);
                        std::vector<uint8_t> order;
                        int v;
                        while (iss >> v) order.push_back((uint8_t)v);
                        if (order.size() == (size_t)count) {
                            cache_and_set_responseb(pduel, order.data(), (int)order.size());
                        } else {
                            uint8_t keep[1] = {0xff};
                            cache_and_set_responseb(pduel, keep, 1);
                        }
                    }
                    break;
                }
                case MSG_RELOAD_FIELD: {
                    uint8_t tp = BufferIO::Read<uint8_t>(pbuf);
                    (void)tp;
                    for(int p = 0; p < 2; ++p) {
                        BufferIO::Read<uint32_t>(pbuf);
                        for(int seq = 0; seq < 7; ++seq) {
                            int val = BufferIO::Read<uint8_t>(pbuf);
                            if(val) pbuf += 2;
                        }
                        for(int seq = 0; seq < 8; ++seq) {
                            int val = BufferIO::Read<uint8_t>(pbuf);
                            if(val) pbuf += 1;
                        }
                        pbuf += 6;
                    }
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    for(int i = 0; i < count; i++) {
                        pbuf += 15;
                    }
                    std::cout << "Reload field\n";
                    needs_display_after = true;
                    break;
                }
                case MSG_AI_NAME: {
                    uint16_t name_len = BufferIO::Read<uint16_t>(pbuf);
                    pbuf += name_len + 1;
                    std::cout << "AI name (len=" << name_len << ")\n";
                    break;
                }
                case MSG_SHOW_HINT: {
                    uint16_t msg_len = BufferIO::Read<uint16_t>(pbuf);
                    pbuf += msg_len + 1;
                    std::cout << "Show hint (len=" << msg_len << ")\n";
                    break;
                }
                case MSG_PLAYER_HINT: {
                    BufferIO::Read<uint8_t>(pbuf);
                    BufferIO::Read<uint32_t>(pbuf);
                    BufferIO::Read<uint16_t>(pbuf);
                    std::cout << "Player hint\n";
                    break;
                }
                case MSG_CARD_HINT: {
                    BufferIO::Read<uint8_t>(pbuf);
                    BufferIO::Read<uint32_t>(pbuf);
                    BufferIO::Read<uint8_t>(pbuf);
                    BufferIO::Read<uint8_t>(pbuf);
                    BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Card hint\n";
                    break;
                }
                case MSG_TAG_SWAP: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    (void)player;
                    uint16_t count1 = BufferIO::Read<uint16_t>(pbuf);
                    uint16_t count2 = BufferIO::Read<uint16_t>(pbuf);
                    pbuf += count1 * 4 + count2 * 4 + 7;
                    std::cout << "Tag swap\n";
                    break;
                }
                case MSG_MATCH_KILL: {
                    BufferIO::Read<uint32_t>(pbuf);
                    std::cout << "Match kill\n";
                    break;
                }
                case MSG_FIELD_DISABLED: {
                    pbuf += 4;
                    std::cout << "\n";
                    break;
                }
                case MSG_SUMMONING:
                case MSG_SPSUMMONING:
                case MSG_FLIPSUMMONING: {
                    pbuf += 8;
                    std::cout << "\n";
                    break;
                }
                case MSG_CHAINED:
                case MSG_CHAIN_SOLVING:
                case MSG_CHAIN_NEGATED:
                case MSG_CHAIN_DISABLED: {
                    pbuf += 1; // chain_flag or player
                    std::cout << "\n";
                    break;
                }
                case MSG_ATTACK_DISABLED: {
                    std::cout << "Attack disabled\n";
                    break;
                }
                case MSG_DAMAGE_STEP_START:
                case MSG_DAMAGE_STEP_END: {
                    // No data
                    break;
                }
                case MSG_MISSED_EFFECT: {
                    pbuf += 8; // player(1) + data(7)
                    std::cout << "\n";
                    break;
                }
                case MSG_HAND_RES: {
                    pbuf += 1; // hand_result
                    std::cout << "\n";
                    break;
                }
                case MSG_SWAP_GRAVE_DECK: {
                    pbuf += 1; // player
                    std::cout << "\n";
                    break;
                }
                case MSG_REVERSE_DECK: {
                    // No data
                    break;
                }
                case MSG_DECK_TOP: {
                    pbuf += 6;
                    std::cout << "\n";
                    break;
                }
                case MSG_SHUFFLE_SET_CARD: {
                    uint8_t count = BufferIO::Read<uint8_t>(pbuf);
                    pbuf += count * 8;
                    std::cout << "\n";
                    break;
                }
                default: {
                    std::cout << " (unhandled message type=" << (int)msg_type << ")\n";
                    // Skip remaining buffer to prevent infinite loop
                    pbuf = msg_buffer + len;
                    break;
                }
            }

                if (display_state_before && !needs_display_after) {
                    // Already displayed before options
                } else if (needs_display_after && !auto_play) {
                    display_game_state();
                }
            }
        }
    }

    // Cleanup
    if (pduel) {
        end_duel(pduel);
    }
    if (db) {
        sqlite3_close(db);
    }

    return 0;
}
