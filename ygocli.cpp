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

// Path of the running binary (set in main), used to re-exec for ygo_server /
// ygo_windbot helpers and to locate wiki/, single/, replay/, WindBot/.
static std::string g_exe_dir;
static std::string g_exe_path;

// Child processes managed by the MCP tools (ygo_server / ygo_windbot).
static pid_t g_server_pid = -1;
static pid_t g_windbot_pid = -1;

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
    (void)label;
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
                    BufferIO::Read<uint32_t>(card_p);
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
                    BufferIO::Read<int32_t>(card_p);
                }
                if (flag & QUERY_DEFENSE) {
                    BufferIO::Read<int32_t>(card_p);
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
static int cli_read_until_prompt(int timeout_ms);
static int cli_wait_for_prompt(int timeout_ms, int max_idle);
static std::string cli_render_field();
static std::string mcp_tool_ygo_client(const nlohmann::json& params);

// ygo_choose tool: send choice, continue game (solo in-process or network client)
static std::string mcp_tool_ygo_choose(const nlohmann::json& params) {
    int choice_idx = -1;
    std::vector<int> choice_indices;
    if (params.contains("id") && params["id"].is_number()) choice_idx = params["id"];
    if (params.contains("indices") && params["indices"].is_array()) {
        for (const auto& v : params["indices"])
            if (v.is_number()) choice_indices.push_back(v);
    }

    // Network client path: build response bytes, send CTOS_RESPONSE, read on.
    // The call blocks until input is needed again ("auto-unblock"): if no
    // prompt is pending we wait for the next one without sending anything,
    // and after sending a response we wait for the next prompt. So an agent
    // can connect once and drive the whole game with ygo_choose alone.
    if (net_client_connected && net_client_fd >= 0) {
        net_client_mode = true;
        mcp_begin_capture();
        int r;
        if (!cli_has_prompt) {
            // Not our turn yet (opponent acting, or game not started): wait
            // for a pending prompt instead of erroring, send no choice.
            r = cli_wait_for_prompt(5000, 120);
        } else {
            mcp_send_choice(choice_idx, choice_indices);
            net_client_mode = false;
            net::write_packet(net_client_fd, CTOS_RESPONSE, net_response_buf.data(), net_response_buf.size());
            cli_has_prompt = false;
            r = cli_wait_for_prompt(5000, 120);
        }
        std::string narration = mcp_take_capture();
        mcp_end_capture();
        net_client_mode = false;

        std::ostringstream out;
        if (!narration.empty()) out << narration;
        out << cli_render_field();
        if (r == 1) {
            if (cli_game_over) out << "Game Over: Player " << cli_winner << " wins!\n";
            else out << "Game Over (duel ended without a winner)\n";
            return out.str();
        }
        if (cli_has_prompt) { out << mcp_build_choices(); return out.str(); }
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
static std::string mcp_tool_ygo_client(const nlohmann::json& params);
static std::string mcp_tool_ygo_choose(const nlohmann::json& params);
static std::string mcp_tool_ygo_single_mode(const nlohmann::json& params);
static std::string mcp_tool_card_search(const nlohmann::json& params);
static std::string mcp_tool_ygo_exit(const nlohmann::json& params);
static std::string mcp_tool_ygo_surrender(const nlohmann::json& params);
static std::string mcp_tool_ygo_observe(const nlohmann::json& params);
static std::string mcp_tool_ygo_replay(const nlohmann::json& params);
static std::string mcp_tool_ygo_puzzle(const nlohmann::json& params);
static std::string mcp_tool_ygo_wiki(const nlohmann::json& params);
static std::string mcp_tool_ygo_server(const nlohmann::json& params);
static std::string mcp_tool_ygo_server_exit(const nlohmann::json& params);
static std::string mcp_tool_ygo_windbot(const nlohmann::json& params);
static std::string mcp_tool_ygo_windbot_exit(const nlohmann::json& params);

static const std::vector<ToolDef>& tools_registry() {
    static const std::vector<ToolDef> tools = {
        {"ygo_solo",
         "Start a solo duel in-process (no server): both players driven from this session, "
         "unfiltered god view. Runs until a choice is needed; respond with ygo_choose.",
         tool_schema({{"deck0", "string", "Path to deck0 .ydk file"},
                      {"deck1", "string", "Path to deck1 .ydk file"},
                      {"lp", "integer", "Starting LP (default 8000)"},
                      {"start_hand", "integer", "Starting hand size (default 5)"},
                      {"draw_count", "integer", "Cards drawn per turn (default 1)"},
                      {"rule", "integer", "Master rule version"}}),
         mcp_tool_ygo_single_mode},
        {"ygo_client",
         "Connect to a ygocli/gframe server as a player. First call joins the room, uploads "
         "your deck and becomes Ready; the host auto-starts once both players are in. "
         "Subsequent calls return the next view/prompt; use ygo_choose to respond. On failure "
         "(connection refused, room full, deck rejected, version mismatch, game already "
         "started) returns an error and closes the connection. mode=1 (default) plays a "
         "best-of-3 match with side-deck exchange; mode=0 is a single game.",
         tool_schema({{"host", "string", "Server host (default 127.0.0.1)"},
                      {"port", "integer", "Server port (default 7911)"},
                      {"password", "string", "Room password (host sets it when creating; joiner must match)"},
                      {"create_game", "boolean", "true to create the room as host"},
                      {"deck", "array", "Your deck: path to a .ydk file, or array of card codes"},
                      {"name", "string", "Player name"},
                      {"lp", "integer", "Starting LP (host, default 8000)"},
                      {"start_hand", "integer", "Starting hand (host, default 5)"},
                      {"draw_count", "integer", "Draws per turn (host, default 1)"},
                      {"rule", "integer", "Master rule (host, default 5)"},
                      {"mode", "integer", "0 = single game, 1 = best-of-3 match (host, default 1)"}}),
         mcp_tool_ygo_client},
        {"ygo_choose",
         "Choose an option from the pending prompt and continue the duel (solo or network).",
         tool_schema({{"id", "integer", "The choice index to select (or -1 to pass)"},
                      {"indices", "array", "For multi-select prompts, ordered list of indices"}}),
         mcp_tool_ygo_choose},
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
        {"ygo_exit",
         "Close the current network connection (leave the room).",
         tool_schema({}),
         mcp_tool_ygo_exit},
        {"ygo_surrender",
         "Surrender the current game. Unlike ygo_exit the connection stays up: in a match "
         "the opponent wins the game, players exchange side decks, and the next game starts.",
         tool_schema({}),
         mcp_tool_ygo_surrender},
        {"ygo_observe",
         "Connect to a ygocli/gframe server as an observer: sees both players' public "
         "information but is never asked for choices.",
         tool_schema({{"host", "string", "Server host (default 127.0.0.1)"},
                      {"port", "integer", "Server port (default 7911)"},
                      {"password", "string", "Room password"}}),
         mcp_tool_ygo_observe},
        {"ygo_replay",
         "Play back a saved replay file (.yrp). Returns a narration + final field state. "
         "Replays are auto-saved by the server to replay/YYYYMMDD_HHMMSS.yrp.",
         tool_schema({{"file", "string", "Path to the .yrp replay file"}}),
         mcp_tool_ygo_replay},
        {"ygo_server",
         "Launch a ygocli server for you (same as running `ygocli server`). The server "
         "runs as a child process; stop it with ygo_server_exit.",
         tool_schema({{"port", "integer", "Listen port (default 7911)"},
                      {"bind", "string", "Bind address (default 0.0.0.0)"}}),
         mcp_tool_ygo_server},
        {"ygo_server_exit",
         "Stop the server launched by ygo_server (if any).",
         tool_schema({}),
         mcp_tool_ygo_server_exit},
        {"ygo_windbot",
         "Launch a WindBot AI client (mono WindBot/WindBot.exe) that connects to a server "
         "and plays autonomously. WindBot is not bundled: returns an error until WindBot.exe "
         "is placed in the WindBot/ directory.",
         tool_schema({{"host", "string", "Server host (default 127.0.0.1)"},
                      {"port", "integer", "Server port (default 7911)"},
                      {"name", "string", "Bot player name"},
                      {"deck", "string", "Path to the bot's .ydk deck"}}),
         mcp_tool_ygo_windbot},
        {"ygo_windbot_exit",
         "Stop the WindBot launched by ygo_windbot (if any).",
         tool_schema({}),
         mcp_tool_ygo_windbot_exit},
        {"ygo_puzzle",
         "Play a puzzle from the single/ folder (like ocgcore/gframe single mode): "
         "single/<puzzle>/deck0.ydk, deck1.ydk, optional setup.lua (runs in the duel's lua "
         "context, e.g. Duel.SetLP). Falls back to explicit deck0/deck1 params. Respond with "
         "ygo_choose.",
         tool_schema({{"puzzle", "string", "Puzzle name -> single/<puzzle>/ decks"},
                      {"deck0", "string", "Fallback deck0 path (if no puzzle name)"},
                      {"deck1", "string", "Fallback deck1 path"},
                      {"lp", "integer", "Starting LP (default 8000)"},
                      {"start_hand", "integer", "Starting hand size (default 5)"},
                      {"draw_count", "integer", "Draws per turn (default 1)"}}),
         mcp_tool_ygo_puzzle},
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

// ---------------------------------------------------------------------------
// ygo_server / ygo_server_exit: manage a child `ygocli server` process.
// ---------------------------------------------------------------------------
static std::string mcp_tool_ygo_server(const nlohmann::json& params) {
    if (g_server_pid > 0) {
        return "Error: a ygocli server is already running (pid " + std::to_string(g_server_pid)
               + "); call ygo_server_exit first";
    }
    int port = params.value("port", 7911);
    std::string bind = params.value("bind", "0.0.0.0");
    pid_t pid = fork();
    if (pid < 0) return "Error: fork failed";
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); }
        execl(g_exe_path.c_str(), "ygocli", "server", "--port",
              std::to_string(port).c_str(), "--bind", bind.c_str(), (char*)nullptr);
        _exit(1);
    }
    g_server_pid = pid;
    return "ygocli server launched on " + bind + ":" + std::to_string(port)
           + " (pid " + std::to_string(pid) + ")";
}

static std::string mcp_tool_ygo_server_exit(const nlohmann::json&) {
    if (g_server_pid <= 0) return "No ygocli server running";
    ::kill(g_server_pid, SIGTERM);
    int st = 0;
    waitpid(g_server_pid, &st, 0);
    g_server_pid = -1;
    return "server stopped";
}

// ---------------------------------------------------------------------------
// ygo_windbot / ygo_windbot_exit: manage a WindBot child (mono WindBot/WindBot.exe).
// WindBot is NOT bundled; the tool errors out until WindBot.exe exists.
// ---------------------------------------------------------------------------
static std::string mcp_tool_ygo_windbot(const nlohmann::json& params) {
    if (g_windbot_pid > 0) {
        return "Error: a WindBot is already running (pid " + std::to_string(g_windbot_pid)
               + "); call ygo_windbot_exit first";
    }
    std::string exe = g_exe_dir + "/WindBot/WindBot.exe";
    if (::access(exe.c_str(), R_OK) != 0) {
        return "Error: WindBot not installed (expected " + exe
               + "). Place WindBot.exe there and retry.";
    }
    std::string host = params.value("host", "127.0.0.1");
    int port = params.value("port", 7911);
    std::string name = params.value("name", "WindBot");
    std::string deck = params.value("deck", "");
    pid_t pid = fork();
    if (pid < 0) return "Error: fork failed";
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); }
        if (!deck.empty()) {
            execl("/usr/bin/mono", "mono", exe.c_str(), host.c_str(),
                  std::to_string(port).c_str(), name.c_str(), deck.c_str(), (char*)nullptr);
        } else {
            execl("/usr/bin/mono", "mono", exe.c_str(), host.c_str(),
                  std::to_string(port).c_str(), name.c_str(), (char*)nullptr);
        }
        _exit(1);
    }
    g_windbot_pid = pid;
    return "WindBot launched (pid " + std::to_string(pid) + "): " + host + ":" + std::to_string(port);
}

static std::string mcp_tool_ygo_windbot_exit(const nlohmann::json&) {
    if (g_windbot_pid <= 0) return "No WindBot running";
    ::kill(g_windbot_pid, SIGTERM);
    int st = 0;
    waitpid(g_windbot_pid, &st, 0);
    g_windbot_pid = -1;
    return "WindBot stopped";
}

// ---------------------------------------------------------------------------
// ygo_puzzle: single/ folder puzzle (decks + optional setup.lua) played solo.
// ---------------------------------------------------------------------------
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
        std::cout << tool->fn(args) << "\n";
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

// ============================================================
// Network server (gframe-compatible dedicated server)
// ============================================================

struct ServerPlayer {
    int fd = -1;
    uint8_t type = 0xff;           // seat 0/1 (NETPLAYER_TYPE_PLAYER1/2), 0xff = unassigned
    uint8_t state = 0;             // expected next CTOS_* packet type
    bool ready = false;
    bool deck_ok = false;
    std::vector<uint32_t> deck_main;
    std::vector<uint32_t> deck_extra;
    std::vector<uint32_t> deck_side;
    uint16_t name[20] = {};
};

static ServerPlayer srv_players[2 + 6];   // seats 0-1: players; seats 2-7: observers
static constexpr int SRV_PLAYER_SEATS = 2;
static constexpr int SRV_MAX_SEATS = 8;
static int srv_listen_fd = -1;
static HostInfo srv_host_info;
static bool srv_started = false;
static int srv_last_response = -1; // seat that must respond
static bool srv_duel_abort = false; // a player disconnected -> end the duel
static int srv_tp_player = 0;      // seat that picks turn after RPS
static int srv_duel_stage = 0;
static uint8_t srv_hand_result[2] = {0, 0};

// Match state: best-of-3 with side-deck exchange (HostInfo.mode == 1).
static bool srv_match_mode = false;
static int srv_match_wins[2] = {0, 0};
static int srv_last_winner = -1;        // winner of the finished game (0/1), -1 if abort
static int srv_surrender_winner = -1;   // set when a player surrenders
static std::string srv_room_pass;       // host-set room password (empty = none)

// Replay capture: raw [u32 len][bytes] chunks of the engine message stream
// for the current game; saved to replay/YYYYMMDD_HHMMSS.yrp at game end.
static std::vector<uint8_t> srv_replay_data;
static uint32_t srv_replay_seed[SEED_COUNT] = {};
static void srv_replay_add(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    uint32_t l = (uint32_t)len;
    uint8_t b[4];
    std::memcpy(b, &l, 4);
    srv_replay_data.insert(srv_replay_data.end(), b, b + 4);
    srv_replay_data.insert(srv_replay_data.end(), data, data + len);
}
static void srv_save_replay(int game_index) {
    if (srv_replay_data.empty()) return;
    std::string dir = g_exe_dir + "/replay";
    mkdir(dir.c_str(), 0755);
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char name[64];
    strftime(name, sizeof name, "%Y%m%d_%H%M%S", &tmv);
    std::string path = dir + "/" + name + (game_index > 1 ? "_g" + std::to_string(game_index) : "") + ".yrp";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "replay: cannot write %s\n", path.c_str()); return; }
    fwrite("ygopro", 1, 6, f);
    uint32_t version = 0x12d0;
    fwrite(&version, 4, 1, f);
    fwrite(srv_replay_seed, 4, SEED_COUNT, f);
    fwrite(srv_replay_data.data(), 1, srv_replay_data.size(), f);
    fclose(f);
    fprintf(stderr, "replay saved: %s (%zu bytes)\n", path.c_str(), srv_replay_data.size());
}

static void srv_send(int seat, uint8_t proto, const uint8_t* data, size_t len) {
    if (seat < 0 || seat >= SRV_MAX_SEATS || srv_players[seat].fd < 0) return;
    net::write_packet(srv_players[seat].fd, proto, data, len);
}
static void srv_send_msg(int seat, const uint8_t* offset, const uint8_t* pbuf) {
    srv_send(seat, STOC_GAME_MSG, offset, (size_t)(pbuf - offset));
}
// Broadcast a message to every observer seat (public view).
static void srv_send_obs(const uint8_t* offset, const uint8_t* pbuf) {
    for (int s = SRV_PLAYER_SEATS; s < SRV_MAX_SEATS; ++s)
        if (srv_players[s].fd >= 0 && srv_players[s].type == NETPLAYER_TYPE_OBSERVER)
            srv_send_msg(s, offset, pbuf);
}
static void srv_send_msg_both(const uint8_t* offset, const uint8_t* pbuf) {
    srv_send_msg(0, offset, pbuf);
    srv_send_msg(1, offset, pbuf);
    srv_send_obs(offset, pbuf);
}
static void srv_send_packet(int seat, uint8_t proto) {
    uint8_t msg = proto;
    srv_send(seat, STOC_GAME_MSG, &msg, 1);
}

// Build MSG_UPDATE_DATA (6) for one player+location; sends full info to owner,
// zeroed hidden segments to the opponent (gframe RefreshMzone/RefreshSzone/...).
static void srv_refresh_location(int owner, uint32_t location, uint32_t flag, bool reveal_only_owner) {
    uint8_t qbuf[0x4000];
    uint8_t* q = qbuf;
    BufferIO::Write<uint8_t>(q, MSG_UPDATE_DATA);
    BufferIO::Write<uint8_t>(q, (uint8_t)owner);
    BufferIO::Write<uint8_t>(q, (uint8_t)location);
    flag |= (QUERY_CODE | QUERY_POSITION);
    int len = query_field_card(global_pduel, owner, location, flag, q, 0);
    if (len <= 0) return;
    // owner copy: full
    srv_send_msg(owner, qbuf, q + len);
    if (reveal_only_owner) {
        // opponent copy: zero any facedown / hidden segment.
        // Record layout: int32 clen | uint32 flag | data...
        // Since WriteUpdateData always adds QUERY_CODE|QUERY_POSITION, the
        // position value (c + l<<8 + s<<16 + pos<<24) sits at offset 8
        // (after flag(4) + code(4)).
        int qlen = 0;
        uint8_t* qc = q;
        while (qlen < len) {
            const int clen = BufferIO::Read<int32_t>(qc);  // qc now at flag
            qlen += clen;
            if (clen <= LEN_HEADER) { qc += clen - 4; continue; }
            uint32_t info = 0;
            std::memcpy(&info, qc + 8, sizeof info);
            uint8_t pos = (uint8_t)(info >> 24);
            bool hide = false;
            if (location == LOCATION_HAND) {
                hide = (pos & POS_FACEUP) == 0;
            } else if (location == LOCATION_MZONE || location == LOCATION_SZONE) {
                hide = (pos & POS_FACEDOWN) != 0 && (pos & POS_REVEAL) == 0;
            } else {
                hide = (pos & POS_FACEDOWN) != 0 && (pos & POS_REVEAL) == 0;
            }
            if (hide) std::memset(qc, 0, clen - 4);  // zero flag+data, keep clen
            qc += clen - 4;  // advance to next record (clen already consumed 4)
        }
    }
    srv_send_msg(1 - owner, qbuf, q + len);
    srv_send_obs(qbuf, q + len);   // observers see the public (hidden-omitted) view
}

static void srv_refresh_mzone(int owner) { srv_refresh_location(owner, LOCATION_MZONE, QUERY_ATTACK | QUERY_DEFENSE | QUERY_LEVEL | QUERY_RANK | QUERY_TYPE, true); }
static void srv_refresh_szone(int owner) { srv_refresh_location(owner, LOCATION_SZONE, QUERY_TYPE, true); }
static void srv_refresh_hand(int owner)  { srv_refresh_location(owner, LOCATION_HAND, QUERY_TYPE, true); }
static void srv_refresh_grave(int owner) { srv_refresh_location(owner, LOCATION_GRAVE, QUERY_TYPE, false); }
static void srv_refresh_extra(int owner) { srv_refresh_location(owner, LOCATION_EXTRA, QUERY_TYPE, true); }

// Core message router: gframe single_duel.cpp Analyze() port.
// Returns: 0 = keep processing, 1 = waiting for a response, 2 = duel over.
static int srv_analyze(uint8_t* msgbuffer, unsigned int len) {
    uint8_t* offset;
    uint8_t* pbuf = msgbuffer;
    int player, count;
    while (pbuf - msgbuffer < (int)len) {
        offset = pbuf;
        uint8_t engType = BufferIO::Read<uint8_t>(pbuf);
        switch (engType) {
        case MSG_RETRY: {
            if (std::getenv("YGOCLI_MCP_DEBUG"))
                fprintf(stderr, "srv MSG_RETRY for seat %d\n", srv_last_response);
            srv_send_packet(srv_last_response, MSG_RETRY);
            return 1;
        }
        case MSG_HINT: {
            uint8_t type = BufferIO::Read<uint8_t>(pbuf);
            player = BufferIO::Read<uint8_t>(pbuf);
            BufferIO::Read<int32_t>(pbuf);
            switch (type) {
            case 1: case 2: case 3: case 5:
                srv_send_msg(player, offset, pbuf); break;
            case 4: case 6: case 7: case 8: case 9: case 11:
                srv_send_msg(1 - player, offset, pbuf); break;
            case 10:
                srv_send_msg(0, offset, pbuf);
                srv_send_msg(1, offset, pbuf);
                break;
            }
            break;
        }
        case MSG_WIN: {
            srv_last_winner = BufferIO::Read<uint8_t>(pbuf); // winner
            BufferIO::Read<uint8_t>(pbuf); // reason
            srv_send_msg(0, offset, pbuf);
            srv_send_msg(1, offset, pbuf);
            srv_send_obs(offset, pbuf);
            return 2;
        }
        case MSG_SELECT_BATTLECMD: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 11;
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 8 + 2;
            srv_refresh_mzone(0); srv_refresh_mzone(1);
            srv_refresh_szone(0); srv_refresh_szone(1);
            srv_refresh_hand(0); srv_refresh_hand(1);
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SELECT_IDLECMD: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 7;
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 7;
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 7;
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 7;
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 7;
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 11 + 3;
            srv_refresh_mzone(0); srv_refresh_mzone(1);
            srv_refresh_szone(0); srv_refresh_szone(1);
            srv_refresh_hand(0); srv_refresh_hand(1);
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SELECT_EFFECTYN: {
            player = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 12;
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SELECT_YESNO: {
            player = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 4;
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SELECT_OPTION: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 4;
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SELECT_CARD:
        case MSG_SELECT_TRIBUTE: {
            player = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 3;
            count = BufferIO::Read<uint8_t>(pbuf);
            for (int i = 0; i < count; ++i) {
                uint8_t* pbufw = pbuf;
                BufferIO::Read<int32_t>(pbuf);
                int c = BufferIO::Read<uint8_t>(pbuf);
                BufferIO::Read<uint8_t>(pbuf);
                BufferIO::Read<uint8_t>(pbuf);
                BufferIO::Read<uint8_t>(pbuf);
                if (c != player) BufferIO::Write<int32_t>(pbufw, 0); // hide opponent codes
            }
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SELECT_UNSELECT_CARD: {
            player = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 4;
            count = BufferIO::Read<uint8_t>(pbuf);
            for (int i = 0; i < count; ++i) {
                uint8_t* pbufw = pbuf;
                BufferIO::Read<int32_t>(pbuf);
                int c = BufferIO::Read<uint8_t>(pbuf);
                BufferIO::Read<uint8_t>(pbuf);
                BufferIO::Read<uint8_t>(pbuf);
                BufferIO::Read<uint8_t>(pbuf);
                if (c != player) BufferIO::Write<int32_t>(pbufw, 0);
            }
            count = BufferIO::Read<uint8_t>(pbuf);
            for (int i = 0; i < count; ++i) {
                uint8_t* pbufw = pbuf;
                BufferIO::Read<int32_t>(pbuf);
                int c = BufferIO::Read<uint8_t>(pbuf);
                BufferIO::Read<uint8_t>(pbuf);
                BufferIO::Read<uint8_t>(pbuf);
                BufferIO::Read<uint8_t>(pbuf);
                if (c != player) BufferIO::Write<int32_t>(pbufw, 0);
            }
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SELECT_CHAIN: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 9 + count * 14;
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SELECT_PLACE:
        case MSG_SELECT_DISFIELD: {
            player = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 5;
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SELECT_POSITION: {
            player = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 5;
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SELECT_COUNTER: {
            player = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 4;
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 9;
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SELECT_SUM: {
            pbuf++;
            player = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 6;
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 11;
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 11;
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_SORT_CARD: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 7;
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_CONFIRM_DECKTOP: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 7;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_CONFIRM_EXTRATOP: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 7;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_CONFIRM_CARDS: {
            player = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 1;
            count = BufferIO::Read<uint8_t>(pbuf);
            if (pbuf[5] != LOCATION_DECK) {
                pbuf += count * 7;
                srv_send_msg_both(offset, pbuf);
            } else {
                pbuf += count * 7;
                srv_send_msg(player, offset, pbuf);
            }
            break;
        }
        case MSG_SHUFFLE_DECK: {
            player = BufferIO::Read<uint8_t>(pbuf);
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_SHUFFLE_HAND: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            srv_send_msg(player, offset, pbuf + count * 4);
            for (int i = 0; i < count; ++i) BufferIO::Write<int32_t>(pbuf, 0);
            srv_send_msg(1 - player, offset, pbuf);
            srv_send_obs(offset, pbuf);
            srv_refresh_hand(player);
            break;
        }
        case MSG_SHUFFLE_EXTRA: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            srv_send_msg(player, offset, pbuf + count * 4);
            for (int i = 0; i < count; ++i) BufferIO::Write<int32_t>(pbuf, 0);
            srv_send_msg(1 - player, offset, pbuf);
            srv_send_obs(offset, pbuf);
            srv_refresh_extra(player);
            break;
        }
        case MSG_REFRESH_DECK: {
            pbuf++;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_SWAP_GRAVE_DECK: {
            player = BufferIO::Read<uint8_t>(pbuf);
            srv_send_msg_both(offset, pbuf);
            srv_refresh_grave(player);
            break;
        }
        case MSG_REVERSE_DECK:
            srv_send_msg_both(offset, pbuf);
            break;
        case MSG_DECK_TOP: {
            pbuf += 6;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_SHUFFLE_SET_CARD: {
            uint8_t loc = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 8;
            srv_send_msg_both(offset, pbuf);
            if (loc == LOCATION_MZONE) { srv_refresh_mzone(0); srv_refresh_mzone(1); }
            else { srv_refresh_szone(0); srv_refresh_szone(1); }
            break;
        }
        case MSG_NEW_TURN: {
            srv_refresh_mzone(0); srv_refresh_mzone(1);
            srv_refresh_szone(0); srv_refresh_szone(1);
            srv_refresh_hand(0); srv_refresh_hand(1);
            pbuf++;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_NEW_PHASE: {
            pbuf += 2;
            srv_send_msg_both(offset, pbuf);
            srv_refresh_mzone(0); srv_refresh_mzone(1);
            srv_refresh_szone(0); srv_refresh_szone(1);
            srv_refresh_hand(0); srv_refresh_hand(1);
            break;
        }
        case MSG_MOVE: {
            uint8_t* pbufw = pbuf;
            int pc = pbuf[4], pl = pbuf[5], cc = pbuf[8], cl = pbuf[9];
            uint8_t cp = pbuf[11];
            (void)pc; (void)pl;
            bool hide_code = (cp & POS_FACEDOWN) != 0 && (cp & POS_REVEAL) == 0;
            if (cl & LOCATION_ONFIELD) {
                uint32_t info = 0;
                std::memcpy(&info, pbufw, 4);
                info &= ~(static_cast<uint32_t>(POS_REVEAL) << 24);
                std::memcpy(pbufw, &info, 4);
                cp = (uint8_t)(info >> 24);
            }
            pbuf += 16;
            srv_send_msg(cc, offset, pbuf);
            if (!(cl & (LOCATION_GRAVE | LOCATION_OVERLAY))
                    && ((cl & (LOCATION_DECK | LOCATION_HAND)) || hide_code))
                BufferIO::Write<int32_t>(pbufw, 0);
            srv_send_msg(1 - cc, offset, pbuf);
            srv_send_obs(offset, pbuf);
            if (cl != 0 && (cl & LOCATION_OVERLAY) == 0 && (cl != pl || pc != cc))
                srv_refresh_location(cc, cl, QUERY_TYPE, true);
            break;
        }
        case MSG_POS_CHANGE: {
            int cc = pbuf[4], cl = pbuf[5], pp = pbuf[6], cp = pbuf[7];
            pbuf += 9;
            srv_send_msg_both(offset, pbuf);
            if ((pp & POS_FACEDOWN) && (cp & POS_FACEUP))
                srv_refresh_location(cc, cl, QUERY_TYPE, true);
            break;
        }
        case MSG_SET: {
            // MSG_SET: msg(1) + code(4) + info_location(4). Zero the code for
            // the opponent copy, consume the full 8-byte payload.
            BufferIO::Write<int32_t>(pbuf, 0);
            pbuf += 8;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_SWAP: {
            int c1 = pbuf[4], l1 = pbuf[5], c2 = pbuf[12], l2 = pbuf[13];
            pbuf += 16;
            srv_send_msg_both(offset, pbuf);
            srv_refresh_location(c1, l1, QUERY_TYPE, true);
            srv_refresh_location(c2, l2, QUERY_TYPE, true);
            break;
        }
        case MSG_FIELD_DISABLED: {
            pbuf += 4;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_SUMMONING: {
            pbuf += 8;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_SUMMONED:
            srv_send_msg_both(offset, pbuf);
            srv_refresh_mzone(0); srv_refresh_mzone(1);
            srv_refresh_szone(0); srv_refresh_szone(1);
            break;
        case MSG_SPSUMMONING: {
            uint8_t* pbufw = pbuf;
            int cc = pbuf[4];
            uint8_t cp = pbuf[7];
            bool hide_code = (cp & POS_FACEDOWN) != 0 && (cp & POS_REVEAL) == 0;
            uint32_t info = 0;
            std::memcpy(&info, pbufw, 4);
            info &= ~(static_cast<uint32_t>(POS_REVEAL) << 24);
            std::memcpy(pbufw, &info, 4);
            pbuf += 8;
            srv_send_msg(cc, offset, pbuf);
            if (hide_code) BufferIO::Write<int32_t>(pbufw, 0);
            srv_send_msg(1 - cc, offset, pbuf);
            srv_send_obs(offset, pbuf);
            break;
        }
        case MSG_SPSUMMONED:
            srv_send_msg_both(offset, pbuf);
            srv_refresh_mzone(0); srv_refresh_mzone(1);
            srv_refresh_szone(0); srv_refresh_szone(1);
            break;
        case MSG_FLIPSUMMONING: {
            srv_refresh_location(pbuf[4], pbuf[5], QUERY_TYPE, true);
            pbuf += 8;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_FLIPSUMMONED:
            srv_send_msg_both(offset, pbuf);
            srv_refresh_mzone(0); srv_refresh_mzone(1);
            srv_refresh_szone(0); srv_refresh_szone(1);
            break;
        case MSG_CHAINING: {
            pbuf += 16;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_CHAINED:
            pbuf++;
            srv_send_msg_both(offset, pbuf);
            srv_refresh_mzone(0); srv_refresh_mzone(1);
            srv_refresh_szone(0); srv_refresh_szone(1);
            srv_refresh_hand(0); srv_refresh_hand(1);
            break;
        case MSG_CHAIN_SOLVING: {
            pbuf++;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_CHAIN_SOLVED:
            pbuf++;
            srv_send_msg_both(offset, pbuf);
            srv_refresh_mzone(0); srv_refresh_mzone(1);
            srv_refresh_szone(0); srv_refresh_szone(1);
            srv_refresh_hand(0); srv_refresh_hand(1);
            break;
        case MSG_CHAIN_END:
            srv_send_msg_both(offset, pbuf);
            srv_refresh_mzone(0); srv_refresh_mzone(1);
            srv_refresh_szone(0); srv_refresh_szone(1);
            srv_refresh_hand(0); srv_refresh_hand(1);
            break;
        case MSG_CHAIN_NEGATED:
        case MSG_CHAIN_DISABLED:
            pbuf++;
            srv_send_msg_both(offset, pbuf);
            break;
        case MSG_CARD_SELECTED: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 4;
            break;
        }
        case MSG_RANDOM_SELECTED: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 4;
            srv_send_msg(player, offset, pbuf);
            srv_send_msg(1 - player, offset, pbuf);
            break;
        }
        case MSG_BECOME_TARGET: {
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count * 4;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_DRAW: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            uint8_t* pbufw = pbuf;
            pbuf += count * 4;
            srv_send_msg(player, offset, pbuf);
            for (int i = 0; i < count; ++i) {
                if (!(pbufw[3] & 0x80)) BufferIO::Write<int32_t>(pbufw, 0);
                else pbufw += 4;
            }
            srv_send_msg(1 - player, offset, pbuf);
            break;
        }
        case MSG_DAMAGE: case MSG_RECOVER: case MSG_LPUPDATE: case MSG_PAY_LPCOST: {
            pbuf += 5;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_EQUIP: { pbuf += 8; srv_send_msg_both(offset, pbuf); break; }
        case MSG_UNEQUIP: { pbuf += 4; srv_send_msg_both(offset, pbuf); break; }
        case MSG_CARD_TARGET: case MSG_CANCEL_TARGET: {
            pbuf += 8;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_ADD_COUNTER: case MSG_REMOVE_COUNTER: {
            pbuf += 7;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_ATTACK: { pbuf += 8; srv_send_msg_both(offset, pbuf); break; }
        case MSG_BATTLE: { pbuf += 26; srv_send_msg_both(offset, pbuf); break; }
        case MSG_ATTACK_DISABLED:
        case MSG_DAMAGE_STEP_START:
        case MSG_DAMAGE_STEP_END:
            srv_send_msg_both(offset, pbuf);
            srv_refresh_mzone(0); srv_refresh_mzone(1);
            break;
        case MSG_MISSED_EFFECT: {
            player = pbuf[0];
            pbuf += 8;
            srv_send_msg(player, offset, pbuf);
            break;
        }
        case MSG_TOSS_COIN: case MSG_TOSS_DICE: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += count;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_ROCK_PAPER_SCISSORS: {
            player = BufferIO::Read<uint8_t>(pbuf);
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_HAND_RES: {
            pbuf += 1;
            srv_send_msg_both(offset, pbuf);
            break;
        }
        case MSG_ANNOUNCE_RACE: case MSG_ANNOUNCE_ATTRIB: {
            player = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 5;
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_ANNOUNCE_CARD: case MSG_ANNOUNCE_NUMBER: {
            player = BufferIO::Read<uint8_t>(pbuf);
            count = BufferIO::Read<uint8_t>(pbuf);
            pbuf += 4 * count;
            srv_send_packet(1 - player, MSG_WAITING);
            srv_last_response = player;
            srv_send_msg(player, offset, pbuf);
            return 1;
        }
        case MSG_CARD_HINT: { pbuf += 9; srv_send_msg_both(offset, pbuf); break; }
        case MSG_PLAYER_HINT: { pbuf += 6; srv_send_msg_both(offset, pbuf); break; }
        default:
            // unknown message: skip 1 byte (best-effort, keep loop from spinning)
            fprintf(stderr, "[server] unknown MSG type %d\n", (int)engType);
            break;
        }
    }
    return 0;
}

// forward decl (defined below)
static void srv_await_response();

// Game loop driver: process() until the duel ends, awaiting responses when needed.
static void srv_game_loop() {
    uint8_t engine_buf[SIZE_MESSAGE_BUFFER];
    unsigned int engFlag = 0;
    int engLen = 0;
    while (engFlag != PROCESSOR_END) {
        unsigned int result = process(global_pduel);
        engLen = (int)(result & PROCESSOR_BUFFER_LEN);
        engFlag = result & PROCESSOR_FLAG;
        if (engLen <= 0) continue;
        get_message(global_pduel, engine_buf);
        srv_replay_add(engine_buf, (size_t)engLen);   // replay capture
        if (std::getenv("YGOCLI_MCP_DEBUG")) {
            fprintf(stderr, "srv msg 0x%02x len%d:", (int)engine_buf[0], engLen);
            for (int i = 0; i < engLen && i < 24; ++i) fprintf(stderr, " %02x", engine_buf[i]);
            fprintf(stderr, "\n");
        }
        int stop = srv_analyze(engine_buf, engLen);
        if (stop == 2) break;             // duel over
        if (stop == 1) { srv_await_response(); if (srv_duel_abort || srv_surrender_winner >= 0) break; continue; } // waiting for a response
    }
    // duel ended
    if (std::getenv("YGOCLI_MCP_DEBUG")) {
        fprintf(stderr, "srv game loop exit engFlag=%u engLen=%d abort=%d\n",
                engFlag, engLen, (int)srv_duel_abort);
    }
}

static int srv_read_packet(int seat, uint8_t* pkt_type, std::vector<uint8_t>& payload, int timeout_ms) {
    std::vector<uint8_t> buf;
    int n = net::read_packet(srv_players[seat].fd, buf, timeout_ms);
    if (n < 0) return -1;
    *pkt_type = buf[0];
    payload.assign(buf.begin() + 1, buf.end());
    return (int)payload.size();
}

// Send the join sequence to a seated player (gframe JoinGame tail):
// STOC_JOIN_GAME(HostInfo) + STOC_TYPE_CHANGE + STOC_HS_PLAYER_ENTER for both
// + STOC_HS_PLAYER_CHANGE ready states.
static void srv_send_join(int seat) {
    STOC_JoinGame sjg{};
    sjg.info = srv_host_info;
    net::write_packet(srv_players[seat].fd, STOC_JOIN_GAME, (const uint8_t*)&sjg, sizeof(STOC_JoinGame));

    STOC_TypeChange sctc{};
    sctc.type = (seat == 0 ? 0x10 : 0) | srv_players[seat].type;
    net::write_packet(srv_players[seat].fd, STOC_TYPE_CHANGE, (const uint8_t*)&sctc, 1);

    for (int p = 0; p < 2; ++p) {
        STOC_HS_PlayerEnter ent{};
        std::memcpy(ent.name, srv_players[p].name, sizeof ent.name);
        ent.pos = (uint8_t)p;
        net::write_packet(srv_players[seat].fd, STOC_HS_PLAYER_ENTER, (const uint8_t*)&ent, sizeof ent);
        if (srv_players[p].ready) {
            STOC_HS_PlayerChange ch{};
            ch.status = (uint8_t)((p << 4) | PLAYERCHANGE_READY);
            net::write_packet(srv_players[seat].fd, STOC_HS_PLAYER_CHANGE, (const uint8_t*)&ch, 1);
        }
    }
    STOC_HS_WatchChange wc{};
    wc.watch_count = 0;
    net::write_packet(srv_players[seat].fd, STOC_HS_WATCH_CHANGE, (const uint8_t*)&wc, 2);
}

// Main server: accept 2 players, handshake, then run the duel.
// Play one game of a match: build the duel from the players' current decks and
// drive it to completion (game 1 = after RPS+TP; games 2+ = after side phase).
// swap=true means player 1 (seat 1) goes first. Saves a replay at game end.
// Returns true when the game ended with a legitimate winner (not an abort).
// Play one game of a match: build the duel from the players' current decks and
// drive it to completion (game 1 = after RPS+TP; games 2+ = after side phase).
// swap=true means player 1 (seat 1) goes first. Saves a replay at game end.
// Returns true when the game ended with a legitimate winner (not an abort).
// Parse a CTOS_UPDATE_DECK payload (uint32 mainc, uint32 sidec, then codes)
// into main/extra/side lists, splitting extra-deck monsters by card type.
// Returns false on malformed input.
static bool srv_parse_deck_payload(const std::vector<uint8_t>& payload,
                                   std::vector<uint32_t>& main, std::vector<uint32_t>& extra,
                                   std::vector<uint32_t>& side) {
    main.clear(); extra.clear(); side.clear();
    if (payload.size() < 8) return false;
    uint32_t mainc, sidec;
    std::memcpy(&mainc, payload.data(), 4);
    std::memcpy(&sidec, payload.data() + 4, 4);
    if ((size_t)(mainc + sidec) > payload.size() / 4) return false;
    const uint8_t* dp = payload.data() + 8;
    for (uint32_t i = 0; i < mainc; ++i) {
        uint32_t code; std::memcpy(&code, dp + i * 4, 4);
        auto it = card_datas.find(code);
        bool is_extra = it != card_datas.end() && (it->second.type & TYPES_EXTRA_DECK);
        if (is_extra) extra.push_back(code); else main.push_back(code);
    }
    for (uint32_t i = 0; i < sidec; ++i) {
        uint32_t code; std::memcpy(&code, dp + (mainc + i) * 4, 4);
        side.push_back(code);
    }
    return true;
}

static bool srv_play_one_game(bool swap, int game_index) {
    Deck decks[2];
    decks[0] = Deck{srv_players[0].deck_main, srv_players[0].deck_extra};
    decks[1] = Deck{srv_players[1].deck_main, srv_players[1].deck_extra};
    if (swap) std::swap(decks[0], decks[1]);

    std::mt19937 rng;
    std::random_device rd;
    rng.seed(rd());
    for (int i = 0; i < 2; i++) {
        std::shuffle(decks[i].main.begin(), decks[i].main.end(), rng);
        std::shuffle(decks[i].extra.begin(), decks[i].extra.end(), rng);
    }
    uint32_t seed[SEED_COUNT];
    for (int i = 0; i < SEED_COUNT; i++) seed[i] = rd();
    for (int i = 0; i < SEED_COUNT; i++) srv_replay_seed[i] = seed[i];

    mcp_setup_engine();
    if (global_pduel) { end_duel(global_pduel); global_pduel = 0; }
    global_pduel = create_duel_v2(seed);
    set_player_info(global_pduel, 0, srv_host_info.start_lp ? srv_host_info.start_lp : 8000,
                    srv_host_info.start_hand ? srv_host_info.start_hand : 5,
                    srv_host_info.draw_count ? srv_host_info.draw_count : 1);
    set_player_info(global_pduel, 1, srv_host_info.start_lp ? srv_host_info.start_lp : 8000,
                    srv_host_info.start_hand ? srv_host_info.start_hand : 5,
                    srv_host_info.draw_count ? srv_host_info.draw_count : 1);
    for (int i = 0; i < 2; i++) {
        for (size_t j = 0; j < decks[i].main.size(); j++)
            new_card(global_pduel, decks[i].main[j], i, i, LOCATION_DECK, decks[i].main.size() - 1 - j, POS_FACEDOWN_DEFENSE);
        for (size_t j = 0; j < decks[i].extra.size(); j++)
            new_card(global_pduel, decks[i].extra[j], i, i, LOCATION_EXTRA, j, POS_FACEDOWN_DEFENSE);
    }
    int rule = srv_host_info.duel_rule ? srv_host_info.duel_rule : CURRENT_RULE;
    // MSG_START (hand-built, per gframe)
    uint8_t startbuf[32]{};
    uint8_t* sp = startbuf;
    BufferIO::Write<uint8_t>(sp, MSG_START);
    BufferIO::Write<uint8_t>(sp, 0);
    BufferIO::Write<uint8_t>(sp, (uint8_t)rule);
    BufferIO::Write<int32_t>(sp, srv_host_info.start_lp ? srv_host_info.start_lp : 8000);
    BufferIO::Write<int32_t>(sp, srv_host_info.start_lp ? srv_host_info.start_lp : 8000);
    BufferIO::Write<uint16_t>(sp, (uint16_t)query_field_count(global_pduel, 0, LOCATION_DECK));
    BufferIO::Write<uint16_t>(sp, (uint16_t)query_field_count(global_pduel, 0, LOCATION_EXTRA));
    BufferIO::Write<uint16_t>(sp, (uint16_t)query_field_count(global_pduel, 1, LOCATION_DECK));
    BufferIO::Write<uint16_t>(sp, (uint16_t)query_field_count(global_pduel, 1, LOCATION_EXTRA));
    int seat0_first = swap ? 1 : 0;
    srv_replay_data.clear();
    srv_replay_add(startbuf, 19);          // replay chunk 0 = MSG_START (player 0 copy)
    srv_send(seat0_first, STOC_GAME_MSG, startbuf, 19);
    startbuf[1] = 1;
    srv_send(1 - seat0_first, STOC_GAME_MSG, startbuf, 19);
    srv_refresh_extra(0);
    srv_refresh_extra(1);
    uint32_t opt = (uint32_t)rule << 16;
    start_duel(global_pduel, opt);
    srv_game_loop();
    // game over (win, surrender, or abort)
    if (global_pduel) { end_duel(global_pduel); global_pduel = 0; }
    srv_save_replay(game_index);
    bool ok = !srv_duel_abort && srv_last_winner >= 0;
    srv_surrender_winner = -1;
    return ok;
}

static int run_server(const std::string& bind_ip, uint16_t port) {
    srv_listen_fd = net::listen(bind_ip, port);
    if (srv_listen_fd < 0) { fprintf(stderr, "server: cannot listen on %s:%u\n", bind_ip.c_str(), (unsigned)port); return 1; }
    fprintf(stderr, "ygocli server listening on %s:%u\n", bind_ip.empty() ? "0.0.0.0" : bind_ip.c_str(), (unsigned)port);

    for (int i = 0; i < SRV_MAX_SEATS; ++i) { srv_players[i] = ServerPlayer(); srv_players[i].type = 0xff; }
    bool deck_received[2] = {false, false};
    int host_started = 0;
    srv_match_mode = false;
    srv_match_wins[0] = srv_match_wins[1] = 0;
    srv_room_pass.clear();
    srv_duel_abort = false;
    srv_last_winner = -1;
    srv_surrender_winner = -1;
    srv_started = false;
    srv_duel_stage = DUEL_STAGE_BEGIN;

    auto reject_seat = [&](int seat, uint8_t errmsg, uint32_t code) {
        STOC_ErrorMsg err{}; err.msg = errmsg; err.code = code;
        net::write_packet(srv_players[seat].fd, STOC_ERROR_MSG, (const uint8_t*)&err, 8);
        net::close(srv_players[seat].fd);
        srv_players[seat].fd = -1;
        srv_players[seat].type = 0xff;
        srv_players[seat].state = 0;
        fprintf(stderr, "seat %d rejected (msg=%d)\n", seat, (int)errmsg);
    };

    // ---- Room formation: accept players (seats 0-1) and observers (2-7), ----
    // interleaved with packet processing so create/join is answered promptly.
    while (srv_duel_stage == DUEL_STAGE_BEGIN) {
        for (int seat = 0; seat < SRV_MAX_SEATS; ++seat) {
            if (srv_players[seat].fd >= 0) continue;
            int fd = net::accept(srv_listen_fd);
            if (fd < 0) break; // no more pending right now
            srv_players[seat].fd = fd;
            srv_players[seat].type = (uint8_t)seat;
            srv_players[seat].state = 0xff;
            srv_players[seat].ready = false;
            srv_players[seat].deck_main.clear();
            srv_players[seat].deck_extra.clear();
            srv_players[seat].deck_side.clear();
            fprintf(stderr, "connection accepted (seat %d)\n", seat);
        }

        bool any_activity = false;
        for (int seat = 0; seat < SRV_MAX_SEATS; ++seat) {
            std::vector<uint8_t> payload;
            uint8_t pkt = 0;
            int n = srv_read_packet(seat, &pkt, payload, 50);
            if (n < 0) continue;   // 50ms poll timeout (or disconnect); keep the seat
            any_activity = true;
            fprintf(stderr, "srv pkt seat%d proto 0x%02x len%d state=0x%02x\n", seat, (int)pkt, n, srv_players[seat].state);
            // state gate: after joining, expect specific packet types
            if (srv_players[seat].state != 0xff && srv_players[seat].state != pkt) continue;

            if (pkt == CTOS_PLAYER_INFO && payload.size() >= 40) {
                std::memcpy(srv_players[seat].name, payload.data(), 40);
                srv_players[seat].state = 0xff; // next: create/join
            } else if (pkt == CTOS_CREATE_GAME && payload.size() >= sizeof(HostInfo) + 80) {
                if (seat >= SRV_PLAYER_SEATS) { reject_seat(seat, ERRMSG_JOINERROR, 0); continue; }
                std::memcpy(&srv_host_info, payload.data(), sizeof(HostInfo));
                srv_match_mode = (srv_host_info.mode == 1);
                // room password from the host (UTF-16 field after the 20-char name)
                srv_room_pass.clear();
                for (size_t i = 0; i < 20; ++i) {
                    char c = (char)payload[sizeof(HostInfo) + 40 + i * 2];
                    if (!c) break;
                    srv_room_pass += c;
                }
                if (seat != 0) {
                    std::swap(srv_players[0], srv_players[seat]);
                    seat = 0;
                }
                srv_players[seat].type = NETPLAYER_TYPE_PLAYER1;
                srv_players[seat].state = CTOS_UPDATE_DECK;
                srv_send_join(seat);
            } else if (pkt == CTOS_JOIN_GAME && payload.size() >= 48) {
                uint16_t ver;
                std::memcpy(&ver, payload.data(), 2);
                if (ver != PRO_VERSION) { reject_seat(seat, ERRMSG_VERERROR, PRO_VERSION); continue; }
                // password check (joiner's pass at offset 8)
                if (!srv_room_pass.empty()) {
                    std::string jp;
                    for (size_t i = 0; i < 20; ++i) {
                        char c = (char)payload[8 + i * 2];
                        if (!c) break;
                        jp += c;
                    }
                    if (jp != srv_room_pass) { reject_seat(seat, ERRMSG_JOINERROR, 0); continue; }
                }
                if (seat >= SRV_PLAYER_SEATS) {
                    // Player connection landed in an observer slot: if a player
                    // seat is still free, move them there.
                    int free_seat = -1;
                    for (int s = 0; s < SRV_PLAYER_SEATS; ++s) if (srv_players[s].fd < 0) { free_seat = s; break; }
                    if (free_seat >= 0) {
                        std::swap(srv_players[free_seat], srv_players[seat]);
                        seat = free_seat;
                    }
                    // else: stays in observer slot until HS_TOOBSERVER; a later
                    // player-signal (deck/ready) triggers the room-full error.
                }
                srv_players[seat].type = NETPLAYER_TYPE_PLAYER2;
                // Observer slots (2+) never upload decks; keep the state open so
                // CTOS_HS_TOOBSERVER passes the state gate.
                srv_players[seat].state = (seat >= SRV_PLAYER_SEATS) ? 0xff : CTOS_UPDATE_DECK;
                srv_send_join(seat);
            } else if (pkt == CTOS_UPDATE_DECK) {
                if (!srv_parse_deck_payload(payload, srv_players[seat].deck_main,
                                            srv_players[seat].deck_extra, srv_players[seat].deck_side)) {
                    reject_seat(seat, ERRMSG_DECKERROR, 0);
                    continue;
                }
                if (srv_players[seat].deck_main.size() < 40 || srv_players[seat].deck_main.size() > 60
                        || srv_players[seat].deck_extra.size() > 15) {
                    // Deck rejected: tell the client and keep the server alive
                    // (the client exits the room per ygo_client semantics).
                    reject_seat(seat, ERRMSG_DECKERROR, 0);
                    deck_received[seat] = false;
                    continue;
                }
                if (seat < SRV_PLAYER_SEATS) {
                    deck_received[seat] = true;
                    srv_players[seat].state = CTOS_HS_READY;
                    fprintf(stderr, "player %d deck ok: %zu main / %zu extra\n", seat,
                            srv_players[seat].deck_main.size(), srv_players[seat].deck_extra.size());
                }
            } else if (pkt == CTOS_HS_READY) {
                if (seat >= SRV_PLAYER_SEATS) { reject_seat(seat, ERRMSG_JOINERROR, 0); continue; }
                if (srv_players[seat].ready) continue;
                srv_players[seat].ready = true;
                srv_players[seat].state = 0xff; // open; only host may start
                fprintf(stderr, "player %d ready\n", seat);
                for (int p = 0; p < 2; ++p) {
                    STOC_HS_PlayerChange ch{};
                    ch.status = (uint8_t)((seat << 4) | PLAYERCHANGE_READY);
                    net::write_packet(srv_players[p].fd, STOC_HS_PLAYER_CHANGE, (const uint8_t*)&ch, 1);
                }
            } else if (pkt == CTOS_HS_TOOBSERVER) {
                // Become an observer: if sitting on a player seat while a player
                // seat is free, move to an observer slot first.
                if (seat < SRV_PLAYER_SEATS) {
                    int free_obs = -1;
                    for (int s = SRV_PLAYER_SEATS; s < SRV_MAX_SEATS; ++s)
                        if (srv_players[s].fd < 0) { free_obs = s; break; }
                    if (free_obs >= 0) {
                        std::swap(srv_players[free_obs], srv_players[seat]);
                        seat = free_obs;
                    }
                }
                srv_players[seat].type = NETPLAYER_TYPE_OBSERVER;
                srv_players[seat].state = 0xff;
                fprintf(stderr, "seat %d is now an observer\n", seat);
            } else if (pkt == CTOS_HS_START) {
                if (seat != 0) continue;
                host_started = 1;
                fprintf(stderr, "host start received\n");
            }
        }

        if (host_started && srv_players[0].ready && srv_players[1].ready
                && deck_received[0] && deck_received[1] && !srv_started) {
            // Host pressed start: STOC_DUEL_START + deck counts + RPS.
            net::send_packet(srv_players[0].fd, STOC_DUEL_START);
            net::send_packet(srv_players[1].fd, STOC_DUEL_START);
            uint8_t deckbuff[12];
            uint8_t* dp = deckbuff;
            BufferIO::Write<uint16_t>(dp, (uint16_t)srv_players[0].deck_main.size());
            BufferIO::Write<uint16_t>(dp, (uint16_t)srv_players[0].deck_extra.size());
            BufferIO::Write<uint16_t>(dp, (uint16_t)srv_players[0].deck_side.size());
            BufferIO::Write<uint16_t>(dp, (uint16_t)srv_players[1].deck_main.size());
            BufferIO::Write<uint16_t>(dp, (uint16_t)srv_players[1].deck_extra.size());
            BufferIO::Write<uint16_t>(dp, (uint16_t)srv_players[1].deck_side.size());
            net::write_packet(srv_players[0].fd, STOC_DECK_COUNT, deckbuff, 12);
            uint8_t swapped[12];
            std::memcpy(swapped, deckbuff + 6, 6);
            std::memcpy(swapped + 6, deckbuff, 6);
            net::write_packet(srv_players[1].fd, STOC_DECK_COUNT, swapped, 12);
            srv_duel_stage = DUEL_STAGE_FINGER;
            srv_players[0].state = CTOS_HAND_RESULT;
            srv_players[1].state = CTOS_HAND_RESULT;
            net::send_packet(srv_players[0].fd, STOC_SELECT_HAND);
            net::send_packet(srv_players[1].fd, STOC_SELECT_HAND);
            srv_started = true;
        }
        if (!any_activity) usleep(1000);
    }

    // ---- Game 1: RPS + turn preference. ----
    while (srv_duel_stage == DUEL_STAGE_FINGER || srv_duel_stage == DUEL_STAGE_FIRSTGO) {
        for (int seat = 0; seat < 2; ++seat) {
            std::vector<uint8_t> payload;
            uint8_t pkt = 0;
            int n = srv_read_packet(seat, &pkt, payload, 50);
            if (n < 0) continue;   // 50ms poll timeout (or disconnect); not fatal here
            if (pkt == CTOS_SURRENDER || pkt == CTOS_LEAVE_GAME) {
                // A player surrendered/left before/during RPS: end the room.
                fprintf(stderr, "player %d %s before the duel started\n", seat,
                        pkt == CTOS_SURRENDER ? "surrendered" : "left");
                srv_duel_abort = true;
                srv_duel_stage = DUEL_STAGE_END;
                break;
            }
            if (pkt == CTOS_HAND_RESULT && srv_duel_stage == DUEL_STAGE_FINGER && payload.size() >= 1) {
                srv_hand_result[seat] = payload[0];
                if (srv_hand_result[0] && srv_hand_result[1]) {
                    STOC_HandResult shr{}; shr.res1 = srv_hand_result[0]; shr.res2 = srv_hand_result[1];
                    net::write_packet(srv_players[0].fd, STOC_HAND_RESULT, (const uint8_t*)&shr, 2);
                    shr.res1 = srv_hand_result[1]; shr.res2 = srv_hand_result[0];
                    net::write_packet(srv_players[1].fd, STOC_HAND_RESULT, (const uint8_t*)&shr, 2);
                    if (srv_hand_result[0] == srv_hand_result[1]) {
                        srv_hand_result[0] = srv_hand_result[1] = 0;
                        net::send_packet(srv_players[0].fd, STOC_SELECT_HAND);
                        net::send_packet(srv_players[1].fd, STOC_SELECT_HAND);
                    } else if ((srv_hand_result[0] == 1 && srv_hand_result[1] == 2)
                            || (srv_hand_result[0] == 2 && srv_hand_result[1] == 3)
                            || (srv_hand_result[0] == 3 && srv_hand_result[1] == 1)) {
                        // player 1 wins RPS -> player 1 picks turn
                        srv_tp_player = 1; srv_duel_stage = DUEL_STAGE_FIRSTGO;
                        srv_players[1].state = CTOS_TP_RESULT; srv_players[0].state = 0xff;
                        net::send_packet(srv_players[1].fd, STOC_SELECT_TP);
                    } else {
                        srv_tp_player = 0; srv_duel_stage = DUEL_STAGE_FIRSTGO;
                        srv_players[0].state = CTOS_TP_RESULT; srv_players[1].state = 0xff;
                        net::send_packet(srv_players[0].fd, STOC_SELECT_TP);
                    }
                }
            } else if (pkt == CTOS_TP_RESULT && srv_duel_stage == DUEL_STAGE_FIRSTGO && payload.size() >= 1) {
                bool swap = ((payload[0] && seat == 1) || (!payload[0] && seat == 0));
                srv_duel_stage = DUEL_STAGE_DUELING;
                srv_play_one_game(swap, 1);
                srv_duel_stage = DUEL_STAGE_SIDING;
            }
        }
        usleep(1000);
    }

    // ---- Match continuation: side phase + next games (best-of-3). ----
    int game_index = 1;
    while (srv_match_mode && !srv_duel_abort && srv_last_winner >= 0) {
        srv_match_wins[srv_last_winner]++;
        fprintf(stderr, "game %d over: player %d wins (score %d-%d)\n", game_index,
                srv_last_winner, srv_match_wins[0], srv_match_wins[1]);
        if (srv_match_wins[srv_last_winner] >= 2) break;

        // Side-deck exchange: ask both players for their (possibly new) deck.
        srv_duel_stage = DUEL_STAGE_SIDING;
        net::send_packet(srv_players[0].fd, STOC_CHANGE_SIDE);
        net::send_packet(srv_players[1].fd, STOC_CHANGE_SIDE);
        bool got_deck[2] = {false, false};
        while (!(got_deck[0] && got_deck[1])) {
            for (int seat = 0; seat < 2; ++seat) {
                if (got_deck[seat]) continue;
                std::vector<uint8_t> payload;
                uint8_t pkt = 0;
                int n = srv_read_packet(seat, &pkt, payload, 50);
                if (n < 0) continue;   // 50ms poll timeout; keep waiting
                if (pkt != CTOS_UPDATE_DECK) continue;
                std::vector<uint32_t> nm, ne, ns;
                if (!srv_parse_deck_payload(payload, nm, ne, ns)) continue;
                if (nm.size() < 40 || nm.size() > 60 || ne.size() > 15) {
                    STOC_ErrorMsg err{}; err.msg = ERRMSG_SIDEERROR; err.code = 0;
                    net::write_packet(srv_players[seat].fd, STOC_ERROR_MSG, (const uint8_t*)&err, 8);
                    continue;   // keep waiting for a valid side deck
                }
                srv_players[seat].deck_main = nm;
                srv_players[seat].deck_extra = ne;
                srv_players[seat].deck_side = ns;
                got_deck[seat] = true;
                fprintf(stderr, "player %d side deck ok: %zu main / %zu extra\n", seat,
                        nm.size(), ne.size());
            }
            if (srv_duel_abort) break;
            usleep(1000);
        }
        if (srv_duel_abort) break;

        // Loser of the previous game picks who goes first (gframe match rule).
        int loser = 1 - srv_last_winner;
        net::send_packet(srv_players[loser].fd, STOC_SELECT_TP);
        bool swap = false;
        bool tp_ok = false;
        while (!tp_ok && !srv_duel_abort) {
            std::vector<uint8_t> payload;
            uint8_t pkt = 0;
            int n = srv_read_packet(loser, &pkt, payload, 50);
            if (n < 0) continue;   // 50ms poll timeout; keep waiting
            if (pkt == CTOS_TP_RESULT && payload.size() >= 1) {
                swap = (payload[0] == 1);   // 0 = seat 0 first, 1 = seat 1 first
                tp_ok = true;
            }
            usleep(1000);
        }
        if (srv_duel_abort) break;

        srv_duel_stage = DUEL_STAGE_DUELING;
        srv_last_winner = -1;
        game_index++;
        if (!srv_play_one_game(swap, game_index)) break;
    }

    // ---- Match over (or aborted): notify and close. ----
    net::send_packet(srv_players[0].fd, STOC_DUEL_END);
    net::send_packet(srv_players[1].fd, STOC_DUEL_END);
    for (int seat = 0; seat < SRV_MAX_SEATS; ++seat) {
        if (srv_players[seat].fd >= 0) net::close(srv_players[seat].fd);
    }
    net::close(srv_listen_fd);
    fprintf(stderr, "server: match finished (score %d-%d), closing\n",
            srv_match_wins[0], srv_match_wins[1]);
    return 0;
}

// Receive a response from the expected player and feed it into the engine.
// Called after srv_game_loop() returns 1 (waiting).
static void srv_await_response() {
    // Handle a surrender (or leave) from either player. winner = the player
    // who did NOT surrender. Returns true if the duel should end.
    auto handle_surrender = [](int who) {
        fprintf(stderr, "player %d surrendered\n", who);
        int winner = 1 - who;
        uint8_t win[3] = {MSG_WIN, (uint8_t)winner, 0};
        srv_replay_add(win, 3);            // record the win in the replay stream
        srv_send_msg(0, win, win + 3);
        srv_send_msg(1, win, win + 3);
        srv_send_obs(win, win + 3);
        srv_last_winner = winner;
        srv_surrender_winner = winner;
        srv_last_response = -1;
    };
    while (srv_last_response >= 0) {
        int seat = srv_last_response;
        int other = 1 - seat;
        // Non-blocking check of the OTHER player: they may surrender (or leave)
        // while the acting player is thinking. The acting player's read below
        // blocks, so this poll is what lets a surrender from the bystander in.
        if (srv_players[other].fd >= 0) {
            pollfd op{};
            op.fd = srv_players[other].fd;
            op.events = POLLIN;
            if (::poll(&op, 1, 0) > 0) {
                std::vector<uint8_t> opbuf;
                uint8_t opkt = 0;
                int on = srv_read_packet(other, &opkt, opbuf, 100);
                if (on >= 0 && opkt == CTOS_SURRENDER) { handle_surrender(other); return; }
                if (on >= 0 && opkt == CTOS_LEAVE_GAME) {
                    fprintf(stderr, "player %d left the room\n", other);
                    srv_duel_abort = true;
                    srv_last_response = -1;
                    return;
                }
            }
        }
        std::vector<uint8_t> payload;
        uint8_t pkt = 0;
        int n = srv_read_packet(seat, &pkt, payload, -1);   // blocking read (EOF = disconnect)
        if (n < 0) {
            fprintf(stderr, "server: player %d disconnected\n", seat);
            net::close(srv_players[seat].fd);
            srv_players[seat].fd = -1;
            srv_duel_abort = true;
            srv_last_response = -1;
            return;
        }
        if (std::getenv("YGOCLI_MCP_DEBUG")) {
            fprintf(stderr, "srv resp seat%d proto 0x%02x len%d\n", seat, (int)pkt, n);
            fprintf(stderr, "  bytes:");
            for (uint8_t b : payload) fprintf(stderr, " %02x", b);
            fprintf(stderr, "\n");
        }
        if (pkt == CTOS_RESPONSE) {
            uint8_t resb[SIZE_RETURN_VALUE]{};
            size_t rlen = payload.size() > SIZE_RETURN_VALUE ? SIZE_RETURN_VALUE : payload.size();
            std::memcpy(resb, payload.data(), rlen);
            set_responseb(global_pduel, resb);
            srv_players[seat].state = 0xff;
            srv_game_loop();
            if (srv_duel_abort) return;
            continue;
        }
        if (pkt == CTOS_SURRENDER) { handle_surrender(seat); return; }
        if (pkt == CTOS_LEAVE_GAME) {
            fprintf(stderr, "player %d left the room\n", seat);
            srv_duel_abort = true;
            srv_last_response = -1;
            return;
        }
    }
}

// ============================================================
// Network client (MCP tool ygo_client) — thin renderer
// ============================================================

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
            return 2;
        }
        if (proto == STOC_SELECT_HAND) {
            // auto rock/paper/scissors: seat-dependent value
            uint8_t res = (uint8_t)((net_client_seat == 0) ? 1 : 2);
            std::vector<uint8_t> rr = {res};
            net::write_packet(net_client_fd, CTOS_HAND_RESULT, rr.data(), 1);
            continue;
        }
        if (proto == STOC_SELECT_TP) {
            if (std::getenv("YGOCLI_MCP_DEBUG")) fprintf(stderr, "cli got STOC_SELECT_TP\n");
            std::vector<uint8_t> tp = {0};
            net::write_packet(net_client_fd, CTOS_TP_RESULT, tp.data(), 1);
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
        if (proto == STOC_DUEL_END) return 1;
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
static int cli_connect_and_join(const std::string& host, uint16_t port,
                                bool create_game, const std::vector<uint32_t>& deck_codes,
                                const std::string& name, const std::string& password,
                                int lp, int start_hand, int draw_count, int rule, int mode) {
    net_client_fd = net::connect(host, port);
    if (net_client_fd < 0) return -1;
    net_client_connected = true;
    cli_deck_codes = deck_codes;

    // CTOS_PLAYER_INFO
    std::vector<uint8_t> ni(40, 0);
    utf16_write(ni, 0, name, 20);
    net::write_packet(net_client_fd, CTOS_PLAYER_INFO, ni.data(), 40);

    if (create_game) {
        HostInfo hi{};
        hi.rule = rule;
        hi.mode = (uint8_t)mode;
        hi.duel_rule = (uint8_t)rule;
        hi.no_check_deck = 0;
        hi.no_shuffle_deck = 0;
        hi.start_lp = lp;
        hi.start_hand = (uint8_t)start_hand;
        hi.draw_count = (uint8_t)draw_count;
        hi.time_limit = 0;
        std::vector<uint8_t> cg(sizeof(CTOS_CreateGame), 0);
        std::memcpy(cg.data(), &hi, sizeof(HostInfo));
        utf16_write(cg, sizeof(HostInfo), name, 20);
        utf16_write(cg, sizeof(HostInfo) + 40, password, 20);
        net::write_packet(net_client_fd, CTOS_CREATE_GAME, cg.data(), cg.size());
    } else {
        std::vector<uint8_t> jg(sizeof(CTOS_JoinGame), 0);
        uint16_t ver = PRO_VERSION;
        std::memcpy(jg.data(), &ver, 2);
        uint32_t gameid = 0;
        std::memcpy(jg.data() + 4, &gameid, 4);
        utf16_write(jg, 8, password, 20);
        net::write_packet(net_client_fd, CTOS_JOIN_GAME, jg.data(), jg.size());
    }

    // read STOC_JOIN_GAME + STOC_TYPE_CHANGE to learn our seat; STOC_ERROR_MSG
    // (room full / deck rejected / version / game started) aborts with its code.
    std::vector<uint8_t> buf;
    bool got_type = false;
    while (!got_type) {
        int n = net::read_packet(net_client_fd, buf, 3000);
        if (n < 0) return -1;
        if (buf.empty()) return -1;
        if (buf[0] == STOC_ERROR_MSG && n >= 8) {
            STOC_ErrorMsg err;
            std::memcpy(&err, buf.data() + 1, sizeof err);
            return (int)err.code ? (int)err.code : (int)err.msg;
        }
        if (buf[0] == STOC_TYPE_CHANGE && n >= 1) {
            net_client_seat = buf[1] & 0x0f;  // low nibble = seat
            // NOTE: do not treat the 0x10 "host" bit as create_game: when the
            // joiner connects before the host, the server seats it at 0 and the
            // TYPE_CHANGE carries 0x10 until the host's create swaps seats, which
            // would make the joiner wrongly act as host (sends HS_START, then
            // blocks waiting for the opponent in an 8s loop). The caller's
            // create_game flag is authoritative.
            got_type = true;
        }
    }

    // upload deck (main+extra as one list; server splits by type)
    std::vector<uint8_t> dk = cli_build_deck_payload(cli_deck_codes);
    net::write_packet(net_client_fd, CTOS_UPDATE_DECK, dk.data(), dk.size());

    // ready
    net::send_packet(net_client_fd, CTOS_HS_READY);
    // Host auto-starts once both players are seated (server waits for both ready).
    if (create_game) {
        // wait a moment for the joiner to connect, then send HS_START
        std::vector<uint8_t> hb;
        int t0 = 0;
        while (t0 < 8000) {
            int n = net::read_packet(net_client_fd, hb, 200);
            if (n < 0) { t0 += 200; continue; }
            if (hb.empty()) { t0 += 200; continue; }
            if (hb[0] == STOC_ERROR_MSG) {
                STOC_ErrorMsg err;
                if (n >= 8) std::memcpy(&err, hb.data() + 1, sizeof err);
                return (int)err.code ? (int)err.code : (int)err.msg;
            }
            // HS_PLAYER_CHANGE with the joiner's READY -> start
            if (hb[0] == STOC_HS_PLAYER_CHANGE) { net::send_packet(net_client_fd, CTOS_HS_START); break; }
        }
    }
    return 0;
}

// Human-readable reason for a server rejection (ERRMSG_* codes).
static std::string cli_err_reason(int code) {
    switch (code) {
    case ERRMSG_JOINERROR: return "room full or join rejected";
    case ERRMSG_DECKERROR: return "deck rejected (illegal or invalid)";
    case ERRMSG_SIDEERROR: return "side deck rejected";
    case ERRMSG_VERERROR: return "version mismatch";
    default: return "server error (code " + std::to_string(code) + ")";
    }
}

// ygo_client MCP tool: connect/join, then read until our prompt or "waiting".
static std::string mcp_tool_ygo_client(const nlohmann::json& params) {
    if (!net_client_connected || net_client_fd < 0) {
        std::string host = params.value("host", "127.0.0.1");
        int port = params.value("port", 7911);
        bool create = params.value("create_game", false);
        nlohmann::json deck = params.contains("deck") ? params["deck"] : nlohmann::json(mcp_deck0_path);
        std::string name = params.value("name", "ygocli");
        std::string password = params.value("password", "");
        int lp = params.value("lp", 8000);
        int hand = params.value("start_hand", 5);
        int draw = params.value("draw_count", 1);
        int rule = params.value("rule", CURRENT_RULE);
        int mode = params.value("mode", 1);   // default: best-of-3 match
        std::vector<uint32_t> codes;
        if (!resolve_deck_codes(deck, codes)) return "Error: invalid deck (need a .ydk path or array of card codes)";
        int rc = cli_connect_and_join(host, port, create, codes, name, password,
                                      lp, hand, draw, rule, mode);
        if (rc != 0) {
            // Spec: on failure, return error and stop the connection (exit room).
            net::close(net_client_fd);
            net_client_fd = -1;
            net_client_connected = false;
            if (rc < 0) return "Error: cannot connect to server (" + host + ":" + std::to_string(port) + ")";
            return "Error: " + cli_err_reason(rc) + " - connection closed";
        }
        cli_clear_field();
    }

    net_client_mode = true;
    mcp_begin_capture();
    // Poll with a short timeout so the tool never blocks forever: if the
    // opponent hasn't acted yet, return "waiting" and the agent re-calls.
    int r = cli_read_until_prompt(500);
    std::string narration = mcp_take_capture();
    mcp_end_capture();
    net_client_mode = false;

    std::ostringstream out;
    if (!narration.empty()) out << narration;
    out << cli_render_field();
    if (r == 2) {
        // Server rejected us mid-game (deck rejected / game started). Exit room.
        net::close(net_client_fd);
        net_client_fd = -1;
        net_client_connected = false;
        return "Error: server rejected the connection - closed";
    }
    if (r == 1) {
        if (cli_game_over) out << "Game Over: Player " << cli_winner << " wins!\n";
        else out << "Game Over (duel ended without a winner)\n";
        return out.str();
    }
    // Surface a pending prompt even if the last read timed out (a prompt may
    // have been stored by an earlier read that consumed a whole prompt packet).
    if (cli_has_prompt) { out << mcp_build_choices(); return out.str(); }
    out << "(waiting for opponent or disconnected)\n";
    return out.str();
}

// ygo_exit: close the current network connection (leave the room).
static std::string mcp_tool_ygo_exit(const nlohmann::json&) {
    if (!net_client_connected || net_client_fd < 0) return "No active connection";
    net::close(net_client_fd);
    net_client_fd = -1;
    net_client_connected = false;
    cli_clear_field();
    cli_deck_codes.clear();
    cli_game_over = false;
    cli_winner = -1;
    return "Left the room (connection closed)";
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
    if (r == 2) { net::close(net_client_fd); net_client_fd = -1; net_client_connected = false;
                  return "Error: server rejected the connection - closed"; }
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
            if (n < 0) { net::close(net_client_fd); net_client_fd = -1; net_client_connected = false; return "Error: handshake failed"; }
            if (buf.empty()) continue;
            if (buf[0] == STOC_ERROR_MSG && n >= 8) {
                net::close(net_client_fd); net_client_fd = -1; net_client_connected = false;
                return "Error: " + cli_err_reason(0) + " - connection closed";
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
    if (r == 2) { net::close(net_client_fd); net_client_fd = -1; net_client_connected = false;
                  return "Error: server rejected the connection - closed"; }
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
    // Resolve the executable path/dir early (needed by ygo_server/ygo_windbot
    // and for locating wiki/, single/, replay/ data folders).
    {
        char exe_path[PATH_MAX];
        if (realpath(argv[0], exe_path)) {
            g_exe_path = exe_path;
            size_t slash = g_exe_path.find_last_of('/');
            if (slash != std::string::npos) g_exe_dir = g_exe_path.substr(0, slash);
        }
    }

    // Subcommands: server | mcp | interact. Anything else = usage help.
    if (argc >= 2) {
        std::string cmd = argv[1];
        if (cmd == "server") {
            std::string bind_ip = "0.0.0.0";
            uint16_t port = 7911;
            for (int i = 2; i < argc; i++) {
                if (std::string(argv[i]) == "--port" && i + 1 < argc) {
                    port = (uint16_t)std::strtoul(argv[i + 1], nullptr, 10);
                    i++;
                } else if (std::string(argv[i]) == "--bind" && i + 1 < argc) {
                    bind_ip = argv[i + 1];
                    i++;
                }
            }
            load_card_database("./cards.cdb");
            load_strings_conf("./strings.conf");
            return run_server(bind_ip, port);
        }
        if (cmd == "interact") {
            load_card_database("./cards.cdb");
            load_strings_conf("./strings.conf");
            mcp_interact_loop();
            if (db) { sqlite3_close(db); db = nullptr; }
            return 0;
        }
        if (cmd == "mcp") { mcp_mode = true; }
    }

    if (argc < 3 && !mcp_mode) {
        std::cerr << "Usage: " << argv[0] << " <deck0.ydk> <deck1.ydk> [--auto]\n";
        std::cerr << "       " << argv[0] << " server [--port N] [--bind ip]   launch a duel server\n";
        std::cerr << "       " << argv[0] << " mcp                          launch MCP server (JSON-RPC over stdio)\n";
        std::cerr << "       " << argv[0] << " interact                     interactive mode (same engine as MCP)\n";
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--auto") {
            auto_play = true;
        } else if (std::string(argv[i]) == "--random") {
            auto_play = true;
            random_choices = true;
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
            if (a == "--auto" || a == "--random") continue;
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

    setup_engine_callbacks();

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
                    BufferIO::Read<uint8_t>(pbuf); // player
                    uint8_t location = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t sequence = BufferIO::Read<uint8_t>(pbuf);
                    BufferIO::Read<uint8_t>(pbuf); // prev_pos
                    uint8_t new_pos = BufferIO::Read<uint8_t>(pbuf);
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
                    BufferIO::Read<uint8_t>(pbuf); // player
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
                        BufferIO::Read<int32_t>(pbuf);  // code
                        BufferIO::Read<uint8_t>(pbuf);  // controler
                        BufferIO::Read<uint8_t>(pbuf);  // location
                        BufferIO::Read<uint8_t>(pbuf);  // sequence
                        BufferIO::Read<int32_t>(pbuf);  // description
                    }

                    // Attackable: each is 8 bytes {code:int32, ctrl:u8, loc:u8, seq:u8, direct_attackable:u8}
                    count = BufferIO::Read<uint8_t>(pbuf);
                    total_options += count;
                    for (int i = 0; i < count; ++i) {
                        BufferIO::Read<int32_t>(pbuf);  // code
                        BufferIO::Read<uint8_t>(pbuf);  // controler
                        BufferIO::Read<uint8_t>(pbuf);  // location
                        BufferIO::Read<uint8_t>(pbuf);  // sequence
                        BufferIO::Read<uint8_t>(pbuf);  // direct_attackable
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
