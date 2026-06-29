// Yu-Gi-Oh! Minimal CLI - Complete version
// Usage: ./ygo-mincli <deck0.ydk> <deck1.ydk> [--auto]
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
#include <functional>
#include <cstdlib>
#include <array>
#include <iomanip>
#include <algorithm>
#include <limits>

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
sqlite3* db = nullptr;

void load_card_database(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        return;
    }

    sqlite3_stmt* stmt;
    std::string sql = "SELECT id, name, desc FROM texts";
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

std::string get_card_name(uint32_t id) {
    if (id == 0) return "None";
    if (card_names.count(id)) return card_names[id];
    return "Card-" + std::to_string(id);
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
    if (type & TYPE_MONSTER) parts.push_back("Monster");
    if (type & TYPE_SPELL) parts.push_back("Spell");
    if (type & TYPE_TRAP) parts.push_back("Trap");
    if (type & TYPE_NORMAL) parts.push_back("Normal");
    if (type & TYPE_EFFECT) parts.push_back("Effect");
    if (type & TYPE_RITUAL) parts.push_back("Ritual");
    if (type & TYPE_FUSION) parts.push_back("Fusion");
    if (type & TYPE_SYNCHRO) parts.push_back("Synchro");
    if (type & TYPE_XYZ) parts.push_back("Xyz");
    if (type & TYPE_LINK) parts.push_back("Link");
    if (type & TYPE_PENDULUM) parts.push_back("Pendulum");
    if (type & TYPE_TUNER) parts.push_back("Tuner");
    if (type & TYPE_QUICKPLAY) parts.push_back("Quick-Play");
    if (type & TYPE_CONTINUOUS) parts.push_back("Continuous");
    if (type & TYPE_EQUIP) parts.push_back("Equip");
    if (type & TYPE_FIELD) parts.push_back("Field");
    if (type & TYPE_COUNTER) parts.push_back("Counter");
    if (type & TYPE_FLIP) parts.push_back("Flip");
    if (type & TYPE_TOON) parts.push_back("Toon");
    if (parts.empty()) {
        return "Type=" + std::to_string(type);
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "/";
        out += parts[i];
    }
    return out;
}

std::string get_card_bracket_info(uint32_t code) {
    auto dit = card_datas.find(code);
    std::string effect = normalize_effect_text(card_descs.count(code) ? card_descs[code] : "");
    if (effect.empty()) effect = "-";
    if (dit == card_datas.end()) {
        return "(type:- lv:- atk:- def:- effect:" + effect + ")";
    }
    const auto& d = dit->second;
    uint32_t lv = d.level & 0xff;
    return "(type:" + card_type_brief(d.type)
        + " lv:" + std::to_string(lv)
        + " atk:" + std::to_string(d.attack)
        + " def:" + std::to_string(d.defense)
        + " effect:" + effect + ")";
}

std::string location_name(uint32_t loc) {
    switch (loc) {
        case LOCATION_DECK: return "Deck";
        case LOCATION_HAND: return "Hand";
        case LOCATION_MZONE: return "MZone";
        case LOCATION_SZONE: return "SZone";
        case LOCATION_GRAVE: return "Grave";
        case LOCATION_REMOVED: return "Removed";
        case LOCATION_EXTRA: return "Extra";
        case LOCATION_OVERLAY: return "Overlay";
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
        default: return "?";
    }
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
intptr_t global_pduel = 0;
std::mt19937 choice_rng;
bool choice_rng_initialized = false;

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

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <deck0.ydk> <deck1.ydk> [--auto]\n";
        return 1;
    }

    for (int i = 3; i < argc; i++) {
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

    // Load card database
    load_card_database("./cards.cdb");

    // Load decks
    Deck decks[2];
    decks[0] = load_deck(argv[1]);
    decks[1] = load_deck(argv[2]);

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
                        std::cout << "Re-sending last response\n";
                        if (last_response_type == RESPONSE_I) {
                            std::cout << "Re-sending int response: " << last_response_i << "\n";
                            cache_and_set_responsei(pduel, last_response_i);
                        } else if (last_response_type == RESPONSE_B) {
                            std::cout << "Re-sending buffer response (length: " << last_response_b_length << "): ";
                            for (size_t i = 0; i < last_response_b_length; i++) {
                                std::cout << std::hex << (int)last_response_b[i] << " ";
                            }
                            std::cout << std::dec << "\n";
                            cache_and_set_responseb(pduel, last_response_b.data(), last_response_b_length);
                        }
                    } else {
                        std::cout << "ERROR: No cached response to retry or pduel mismatch\n";
                    }
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
                        pbuf += count * 11;
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
                            pbuf += count * 8;
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
                    for(int i = 0; i < count; i++) {
                        int32_t opt = BufferIO::Read<int32_t>(pbuf);
                        if (!auto_play) {
                            std::cout << "  [" << i << "] Option " << opt << "\n";
                        }
                    }
                    if (!auto_play) {
                        display_game_state();
                    }
                    if (count == 1) {
                        std::cout << "Only one option, auto-selecting 0\n";
                        cache_and_set_responsei(pduel, 0);
                    } else if (auto_play) {
                        cache_and_set_responsei(pduel, count > 0 ? (random_choices ? rand_int_inclusive(0, count - 1) : 0) : -1);
                    } else {
                        std::cout << "Your choice (-1 to cancel, 0-" << (int)(count-1) << "): ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        try { int choice = std::stoi(line); cache_and_set_responsei(pduel, choice); }
                        catch (...) { cache_and_set_responsei(pduel, -1); }
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
                    if (!auto_play) {
                        display_game_state();
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
                            std::cout << "Your choice (space/comma-separated indices, -1 to cancel): ";
                            std::cout.flush();
                            std::string line;
                            std::getline(std::cin, line);
                            try {
                                int cancel_choice = std::stoi(line);
                                if(cancel_choice == -1) {
                                    cache_and_set_responsei(pduel, -1);
                                    break;
                                }
                            } catch (...) {}
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
                                indices.clear();
                                for(int i = 0; i < min && i < count; ++i) {
                                    indices.push_back(static_cast<uint8_t>(i));
                                }
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
                    if (!auto_play) {
                        display_game_state();
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
                    
                    // Show selectable options
                    struct PlaceOption {
                        int ctrl;
                        int loc;
                        int seq;
                        std::string name;
                    };
                    std::vector<PlaceOption> options;
                    
                    // Check player 0 MZONE (bits 0-6)
                    for (int i = 0; i < 7; i++) {
                        if (selectable & (0x1U << i)) {
                            options.push_back({0, LOCATION_MZONE, i, "P0-MZone[" + std::to_string(i) + "]"});
                        }
                    }
                    // Check player 0 SZONE (bits 8-15)
                    for (int i = 0; i < 8; i++) {
                        if (selectable & (0x100U << i)) {
                            options.push_back({0, LOCATION_SZONE, i, "P0-SZone[" + std::to_string(i) + "]"});
                        }
                    }
                    // Check player 1 MZONE (bits 16-22)
                    for (int i = 0; i < 7; i++) {
                        if (selectable & (0x10000U << i)) {
                            options.push_back({1, LOCATION_MZONE, i, "P1-MZone[" + std::to_string(i) + "]"});
                        }
                    }
                    // Check player 1 SZONE (bits 24-31)
                    for (int i = 0; i < 8; i++) {
                        if (selectable & (0x1000000U << i)) {
                            options.push_back({1, LOCATION_SZONE, i, "P1-SZone[" + std::to_string(i) + "]"});
                        }
                    }

                    if (!auto_play) {
                        display_game_state();
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
                        // No options, just send something
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
                    if (!auto_play) {
                        display_game_state();
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
                        std::cout << "Your choice: ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
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
                    if (!auto_play) {
                        display_game_state();
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
                        std::cout << "Your choice (-1 to cancel, 0-" << (int)(select_count-1) << "): ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        cache_and_set_responsei(pduel, -1);
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
                    display_game_state();
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
                case MSG_HINT: {
                    uint8_t type = BufferIO::Read<uint8_t>(pbuf);
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    int32_t data = BufferIO::Read<int32_t>(pbuf);
                    std::cout << "Hint: type=" << (int)type << ", player=" << (int)player << ", data=" << data << "\n";
                    break;
                }
                case MSG_SHUFFLE_DECK: {
                    uint8_t player = BufferIO::Read<uint8_t>(pbuf);
                    std::cout << "Player " << (int)player << " shuffles deck\n";
                    needs_display_after = true;
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
                        cache_and_set_responsei(pduel, 0);
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
                        std::cout << "Your choice (bitmask): ";
                        std::cout.flush();
                        std::string line;
                        std::getline(std::cin, line);
                        cache_and_set_responsei(pduel, 0);
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
