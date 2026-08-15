// game.cpp — business logic (see game.h). The protocol knowhows learned from
// the wire-layer work are documented at the exact code they affect.

#include "game.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>

namespace game {

using Json = nlohmann::json;

// Thread-safe-ish RNG for the automatic pre-duel answers (RPS / TP). Seeded
// from the system random device — CAVEAT: std::rand() without seeding gives
// every process the SAME sequence, so two clients always tie on RPS forever.
namespace {
std::mt19937& Rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
}  // namespace

// ---------------------------------------------------------------------------
// CardDb
// ---------------------------------------------------------------------------

CardDb::~CardDb() {
    if (db_)
        sqlite3_close(static_cast<sqlite3*>(db_));
}

bool CardDb::Open(const std::string& path, std::string& err) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        err = "cannot open card db: " + path;
        db_ = nullptr;
        return false;
    }
    db_ = db;
    return true;
}

bool CardDb::Get(uint32_t code, Card& out) const {
    auto it = cache_.find(code);
    if (it != cache_.end()) {
        out = it->second;
        return true;
    }
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT datas.id, texts.name, texts.desc, datas.type, datas.atk, datas.def, "
        "datas.level, datas.race, datas.attribute FROM datas INNER JOIN texts ON "
        "datas.id = texts.id WHERE datas.id = ?";
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, static_cast<int>(code));
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out.code = static_cast<uint32_t>(sqlite3_column_int(st, 0));
        out.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        out.text = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        out.type = static_cast<uint32_t>(sqlite3_column_int(st, 3));
        out.attack = sqlite3_column_int(st, 4);
        out.defense = sqlite3_column_int(st, 5);
        out.level = static_cast<uint32_t>(sqlite3_column_int(st, 6));
        out.race = static_cast<uint32_t>(sqlite3_column_int(st, 7));
        out.attribute = static_cast<uint32_t>(sqlite3_column_int(st, 8));
        cache_[code] = out;
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

std::vector<Card> CardDb::Search(const std::string& q, size_t limit) const {
    std::vector<Card> out;
    if (q.empty())
        return out;
    // exact code
    char* end = nullptr;
    long code = std::strtol(q.c_str(), &end, 10);
    if (end && *end == '\0' && code > 0) {
        Card c;
        if (Get(static_cast<uint32_t>(code), c))
            out.push_back(c);
        return out;
    }
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT datas.id, texts.name, texts.desc, datas.type, datas.atk, datas.def, "
        "datas.level, datas.race, datas.attribute FROM datas INNER JOIN texts ON "
        "datas.id = texts.id WHERE texts.name LIKE ? LIMIT ?";
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_text(st, 1, ("%" + q + "%").c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, static_cast<int>(limit));
    while (sqlite3_step(st) == SQLITE_ROW) {
        Card c;
        c.code = static_cast<uint32_t>(sqlite3_column_int(st, 0));
        c.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        c.text = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        c.type = static_cast<uint32_t>(sqlite3_column_int(st, 3));
        c.attack = sqlite3_column_int(st, 4);
        c.defense = sqlite3_column_int(st, 5);
        c.level = static_cast<uint32_t>(sqlite3_column_int(st, 6));
        c.race = static_cast<uint32_t>(sqlite3_column_int(st, 7));
        c.attribute = static_cast<uint32_t>(sqlite3_column_int(st, 8));
        out.push_back(std::move(c));
    }
    sqlite3_finalize(st);
    return out;
}

// ---------------------------------------------------------------------------
// Game lifecycle
// ---------------------------------------------------------------------------

Game::Game(proto::Connection& conn) : conn_(conn) {}

bool Game::OpenCardDb(const std::string& path, std::string& err) {
    return cards_.Open(path, err);
}

bool Game::SendFrame(const std::vector<uint8_t>& frame) {
    std::string err;
    if (!conn_.SendFrame(frame, err)) {
        LogNarration("[!] send failed: " + err);
        return false;
    }
    return true;
}

bool Game::Start(const std::string& host, uint16_t port, const std::string& room,
                 const std::string& name, const std::string& deckfile, std::string& err) {
    // Deck load (ydk: main/extra/side split on !extra / !side markers).
    std::vector<uint32_t> main_cards, extra_cards, side_cards;
    {
        std::ifstream f(deckfile);
        if (!f) {
            err = "cannot open deck: " + deckfile;
            return false;
        }
        enum Section { S_MAIN, S_EXTRA, S_SIDE } sec = S_MAIN;
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("!extra", 0) == 0) { sec = S_EXTRA; continue; }
            if (line.rfind("!side", 0) == 0) { sec = S_SIDE; continue; }
            if (line.empty() || line[0] == '#')
                continue;
            char* end = nullptr;
            long v = std::strtol(line.c_str(), &end, 10);
            if (end == line.c_str())
                continue;
            uint32_t code = static_cast<uint32_t>(v);
            if (sec == S_EXTRA)
                extra_cards.push_back(code);
            else if (sec == S_SIDE)
                side_cards.push_back(code);
            else
                main_cards.push_back(code);
        }
        if (main_cards.empty()) {
            err = "deck has no main cards";
            return false;
        }
    }
    name_ = name;
    room_ = room;
    deckfile_ = deckfile;

    if (!conn_.Connect(host, port, err))
        return false;
    if (!SendFrame(proto::CtosPlayerInfo(name_)))
        return false;
    if (!SendFrame(proto::CtosJoinGame(room_)))
        return false;
    LogNarration("[join] " + name_ + " -> " + room_);

    // The deck/ready are sent once the server confirms the join (STOC_JOIN_GAME)
    // — the reference sends them when the lobby appears.
    std::vector<uint32_t> me = main_cards;
    me.insert(me.end(), extra_cards.begin(), extra_cards.end());
    deck_main_extra_ = std::move(me);
    deck_side_ = std::move(side_cards);
    return true;
}

bool Game::SendDeckAndReady() {
    if (!SendFrame(proto::CtosUpdateDeck(deck_main_extra_, deck_side_)))
        return false;
    if (!SendFrame(proto::CtosNoData(proto::CtosType::HsReady)))
        return false;
    joined_ = true;
    LogNarration("[lobby] deck uploaded, ready");
    return true;
}

bool Game::IsConnected() const { return conn_.IsOpen(); }

bool Game::ReadAndDispatch(std::string& err) {
    std::vector<proto::StocPacket> pkts;
    std::vector<std::vector<uint8_t>> raws;
    if (!conn_.ReadPackets(pkts, raws, err))
        return false;
    // Two-pass batch processing (CAVEAT from the engine's state machine): the
    // engine sets the prompted player's state to CTOS_TIME_CONFIRM and silently
    // drops a CTOS_RESPONSE arriving before the CTOS_TIME_CONFIRM. TIME_LIMIT
    // and the prompt frame may arrive in either order in one batch, so ALL
    // non-prompt packets must be handled before any prompt response is sent.
    for (auto& pkt : pkts)
        if (pkt.type != proto::StocType::GameMsg)
            HandlePacket(pkt);
    for (auto& pkt : pkts)
        if (pkt.type == proto::StocType::GameMsg)
            HandlePacket(pkt);
    return true;
}

// ---------------------------------------------------------------------------
// Packet dispatch
// ---------------------------------------------------------------------------

void Game::HandlePacket(const proto::StocPacket& pkt) {
    using namespace proto;
    if (std::getenv("YGOMCP_DEBUG"))
        std::fprintf(stderr, "[dbg] stoc type=%d\n", static_cast<int>(pkt.type));
    switch (pkt.type) {
    case StocType::GameMsg: {
        const auto& msgs = std::get<std::vector<MsgEvent>>(pkt.body);
        for (const auto& e : msgs)
            HandleMsg(e);
        break;
    }
    case StocType::ErrorMsg: {
        const auto& m = std::get<StocErrorMsg>(pkt.body);
        LogNarration("[!] server error msg=" + std::to_string(m.msg) +
                     " code=0x" + [&] {
                         char b[16];
                         std::snprintf(b, sizeof(b), "%x", m.code);
                         return std::string(b);
                     }());
        break;
    }
    case StocType::SelectHand: {
        // RPS: hands are 1..3 (1 rock, 2 paper, 3 scissors) — res==0 is
        // silently rejected by the engine (stalls the pre-duel forever).
        uint8_t hand = 1 + static_cast<uint8_t>(Rng()() % 3);
        SendFrame(CtosHandResult(hand));
        break;
    }
    case StocType::SelectTp: {
        SendFrame(CtosTpResult(static_cast<uint8_t>(Rng()() & 1)));
        break;
    }
    case StocType::ChangeSide: {
        SendFrame(CtosUpdateDeck(deck_main_extra_, deck_side_));
        break;
    }
    case StocType::TimeLimit: {
        // CAVEAT: with time_limit > 0, EVERY prompt is preceded by STOC_TIME_LIMIT
        // and the prompted player MUST reply CTOS_TIME_CONFIRM before their real
        // response is accepted.
        const auto& m = std::get<StocTimeLimit>(pkt.body);
        if (m.player == static_cast<uint8_t>(my_type_ & 0x0f))
            SendFrame(CtosNoData(CtosType::TimeConfirm));
        break;
    }
    case StocType::TypeChange: {
        // Bit 4 set = host; low nibble = seat.
        my_type_ = std::get<StocTypeChange>(pkt.body).type;
        break;
    }
    case StocType::JoinGame: {
        if (!joined_)
            SendDeckAndReady();
        break;
    }
    case StocType::HsPlayerChange: {
        const auto& m = std::get<StocHsPlayerChange>(pkt.body);
        uint8_t pos = m.status >> 4;
        uint8_t state = m.status & 0x0f;
        if (pos < 4)
            ready_[pos] = state == PlayerChangeReady;
        // Host starts the duel once BOTH seats are ready (the backend ignores
        // HS_START before that; the room does not auto-start).
        if ((my_type_ & 0x10) && ready_[0] && ready_[1] && !started_) {
            started_ = true;
            SendFrame(CtosNoData(CtosType::HsStart));
            LogNarration("[lobby] host start");
        }
        break;
    }
    case StocType::DuelStart:
        LogNarration("[lobby] duel start");
        break;
    case StocType::DuelEnd:
        if (!finished_) {
            finished_ = true;
            LogNarration("[game] duel end");
        }
        break;
    case StocType::Chat: {
        const auto& m = std::get<StocChat>(pkt.body);
        std::string s;
        for (auto c : m.msg)
            s.push_back(static_cast<char>(c & 0x7f));
        chat_.push_back(s);
        break;
    }
    default:
        break;  // informational
    }
}

// ---------------------------------------------------------------------------
// MSG handling
// ---------------------------------------------------------------------------

namespace {
const char* PhaseName(uint16_t phase) {
    switch (phase) {
    case proto::PhaseDraw: return "Draw";
    case proto::PhaseStandby: return "Standby";
    case proto::PhaseMain1: return "Main 1";
    case proto::PhaseBattleStart: return "Battle";
    case proto::PhaseBattleStep: return "Battle step";
    case proto::PhaseDamage: return "Damage";
    case proto::PhaseDamageCalc: return "Damage calc";
    case proto::PhaseBattle: return "Battle";
    case proto::PhaseMain2: return "Main 2";
    case proto::PhaseEnd: return "End";
    default: return "?";
    }
}
std::string CardName(CardDb& db, uint32_t code) {
    Card c;
    if (db.Get(code, c))
        return c.name;
    return "[" + std::to_string(code) + "]";
}
}  // namespace

void Game::LogNarration(const std::string& line) {
    if (log_.size() >= 500)
        log_.erase(log_.begin(), log_.begin() + (log_.size() - 500));
    log_.push_back(line);
}

void Game::OnDraw(const proto::MsgDraw& m) {
    board_.p[m.player].hand += static_cast<int>(m.codes.size());
    std::string names;
    for (size_t i = 0; i < m.codes.size(); ++i) {
        if (i)
            names += ", ";
        names += CardName(cards_, m.codes[i]);
    }
    LogNarration("[T" + std::to_string(board_.turn) + "] draw: " + names);
}

void Game::OnUpdateData(const proto::MsgUpdateData& m) {
    // Rebuild the board zone from the server's card data. Empty slots (len<=8)
    // were skipped by the parser.
    if (m.location == proto::LocMzone || m.location == proto::LocSzone) {
        auto& zone = m.location == proto::LocMzone ? board_.p[m.player].mzone
                                                   : board_.p[m.player].szone;
        zone.clear();
        for (const auto& ci : m.cards) {
            BoardCard bc;
            bc.code = ci.code;
            bc.position = static_cast<uint8_t>(ci.position >> 24);
            bc.face_up = (ci.position & 0x08) != 0 || (ci.position & 0x01) != 0;
            zone.push_back(bc);
        }
    } else if (m.location == proto::LocHand) {
        board_.p[m.player].hand = static_cast<int>(m.cards.size());
    } else if (m.location == proto::LocGrave) {
        board_.p[m.player].grave = static_cast<int>(m.cards.size());
    } else if (m.location == proto::LocRemoved) {
        board_.p[m.player].removed = static_cast<int>(m.cards.size());
    } else if (m.location == proto::LocDeck) {
        board_.p[m.player].deck = static_cast<int>(m.cards.size());
    } else if (m.location == proto::LocExtra) {
        board_.p[m.player].extra = static_cast<int>(m.cards.size());
    }
}

void Game::OnUpdateCard(const proto::MsgUpdateCard& m) {
    if (m.location == proto::LocMzone || m.location == proto::LocSzone) {
        auto& zone = m.location == proto::LocMzone ? board_.p[m.player].mzone
                                                   : board_.p[m.player].szone;
        if (m.sequence < zone.size()) {
            zone[m.sequence].code = m.card.code;
            zone[m.sequence].position = static_cast<uint8_t>(m.card.position >> 24);
            zone[m.sequence].face_up =
                (m.card.position & 0x08) != 0 || (m.card.position & 0x01) != 0;
        }
    }
}

void Game::OnMove(const proto::MsgMove& m) {
    // Moderate board tracking: adjust location counts; field zones are rebuilt
    // by UPDATE_DATA (authoritative) — MOVE adjusts counts only.
    auto& prev = m.prev_location;
    auto& cur = m.cur_location;
    auto count = [&](uint8_t loc) -> int& {
        PlayerBoard& p = board_.p[m.cur_controler];
        switch (loc) {
        case proto::LocDeck: return p.deck;
        case proto::LocHand: return p.hand;
        case proto::LocGrave: return p.grave;
        case proto::LocRemoved: return p.removed;
        case proto::LocExtra: return p.extra;
        default: {
            static int dummy = 0;
            return dummy;
        }
        }
    };
    if (prev != cur) {
        if (count(prev) > 0)
            count(prev)--;
        count(cur)++;
    }
}

void Game::HandleMsg(const proto::MsgEvent& e) {
    using namespace proto;
    if (std::getenv("YGOMCP_DEBUG"))
        std::fprintf(stderr, "[msg] %d\n", e.code);
    switch (static_cast<MsgCode>(e.code)) {
    case MsgCode::MsgStart: {
        const auto& m = std::get<MsgStart>(e.payload);
        board_.p[0].lp = m.lp[0];
        board_.p[1].lp = m.lp[1];
        board_.p[0].deck = m.deck[0];
        board_.p[0].extra = m.extra[0];
        board_.p[1].deck = m.deck[1];
        board_.p[1].extra = m.extra[1];
        board_.turn = 1;
        break;
    }
    case MsgCode::MsgNewTurn: {
        const auto& m = std::get<MsgNewTurn>(e.payload);
        board_.turn++;
        LogNarration("[T" + std::to_string(board_.turn) + "] " +
                     std::string(m.player == 0 ? "you" : "opp") + "'s turn");
        break;
    }
    case MsgCode::MsgNewPhase: {
        const auto& m = std::get<MsgNewPhase>(e.payload);
        board_.phase = m.phase;
        LogNarration("[T" + std::to_string(board_.turn) + "] " + PhaseName(m.phase));
        break;
    }
    case MsgCode::MsgDraw: OnDraw(std::get<MsgDraw>(e.payload)); break;
    case MsgCode::MsgUpdateData: OnUpdateData(std::get<MsgUpdateData>(e.payload)); break;
    case MsgCode::MsgUpdateCard: OnUpdateCard(std::get<MsgUpdateCard>(e.payload)); break;
    case MsgCode::MsgMove: OnMove(std::get<MsgMove>(e.payload)); break;
    case MsgCode::MsgLpUpdate: {
        const auto& m = std::get<MsgLp>(e.payload);
        board_.p[m.player].lp = m.value;
        break;
    }
    case MsgCode::MsgDamage: {
        const auto& m = std::get<MsgLp>(e.payload);
        board_.p[m.player].lp -= m.value;
        LogNarration("[T" + std::to_string(board_.turn) + "] " +
                     std::string(m.player == 0 ? "you" : "opp") + " takes " +
                     std::to_string(m.value) + " damage (LP " +
                     std::to_string(board_.p[m.player].lp) + ")");
        break;
    }
    case MsgCode::MsgRecover: {
        const auto& m = std::get<MsgLp>(e.payload);
        board_.p[m.player].lp += m.value;
        LogNarration("[T" + std::to_string(board_.turn) + "] " +
                     std::string(m.player == 0 ? "you" : "opp") + " recovers " +
                     std::to_string(m.value) + " (LP " +
                     std::to_string(board_.p[m.player].lp) + ")");
        break;
    }
    case MsgCode::MsgPayLpCost: {
        const auto& m = std::get<MsgLp>(e.payload);
        board_.p[m.player].lp -= m.value;
        break;
    }
    case MsgCode::MsgSummoning:
    case MsgCode::MsgSpSummoning:
    case MsgCode::MsgFlipSummoning: {
        const auto& m = std::get<MsgSummoning>(e.payload);
        LogNarration("[T" + std::to_string(board_.turn) + "] " +
                     CardName(cards_, m.code) +
                     (e.code == static_cast<uint8_t>(MsgCode::MsgSpSummoning) ? " special-summoned" :
                      e.code == static_cast<uint8_t>(MsgCode::MsgFlipSummoning) ? " flip-summoned" : " summoned"));
        break;
    }
    case MsgCode::MsgChaining: {
        const auto& m = std::get<MsgChaining>(e.payload);
        LogNarration("[T" + std::to_string(board_.turn) + "] chain " +
                     std::to_string(m.chain_count) + ": " + CardName(cards_, m.code) +
                     " activates");
        break;
    }
    case MsgCode::MsgChainSolved: {
        const auto& m = std::get<MsgChainCount>(e.payload);
        LogNarration("chain " + std::to_string(m.chain_count) + " resolved");
        break;
    }
    case MsgCode::MsgChainNegated:
    case MsgCode::MsgChainDisabled: {
        const auto& m = std::get<MsgChainCount>(e.payload);
        LogNarration("chain " + std::to_string(m.chain_count) + " negated");
        break;
    }
    case MsgCode::MsgChainEnd:
        LogNarration("chain end");
        break;
    case MsgCode::MsgAttack: {
        (void)std::get<MsgAttack>(e.payload);
        LogNarration("[T" + std::to_string(board_.turn) + "] attack declared");
        break;
    }
    case MsgCode::MsgBattle: {
        const auto& m = std::get<MsgBattle>(e.payload);
        int a = m.a_atk, d = m.d_atk;
        LogNarration("battle " + std::to_string(a) + " vs " + std::to_string(d));
        break;
    }
    case MsgCode::MsgWin: {
        const auto& m = std::get<MsgWin>(e.payload);
        finished_ = true;
        last_win_ = m.player == 0 ? "you win" : m.player == 1 ? "opponent wins"
                                                              : "draw";
        LogNarration("[game] " + last_win_ + " (reason " + std::to_string(m.reason) + ")");
        break;
    }
    case MsgCode::MsgRetry:
        ++retries_;
        LogNarration("[!] retry — re-answering last prompt");
        break;
    default:
        break;
    }
    // Prompt builders (after narration so the log reads naturally).
    switch (static_cast<MsgCode>(e.code)) {
    case MsgCode::MsgSelectYesNo: BuildYesNo(std::get<MsgSelectYesNo>(e.payload)); break;
    case MsgCode::MsgSelectEffectYn: BuildYesNo(MsgSelectYesNo{std::get<MsgSelectEffectYn>(e.payload).player, std::get<MsgSelectEffectYn>(e.payload).desc}); break;
    case MsgCode::MsgSelectOption: BuildOption(std::get<MsgSelectOption>(e.payload)); break;
    case MsgCode::MsgSelectChain: BuildChain(std::get<MsgSelectChain>(e.payload)); break;
    case MsgCode::MsgSelectCard: BuildCardSelect(std::get<MsgSelectCard>(e.payload), Prompt::kCardSelect); break;
    case MsgCode::MsgSelectTribute: BuildCardSelect(MsgSelectCard{std::get<MsgSelectTribute>(e.payload).player, std::get<MsgSelectTribute>(e.payload).cancelable, std::get<MsgSelectTribute>(e.payload).min, std::get<MsgSelectTribute>(e.payload).max, std::get<MsgSelectTribute>(e.payload).cards}, Prompt::kCardSelect); break;
    case MsgCode::MsgSelectUnselectCard: BuildUnselect(std::get<MsgSelectUnselectCard>(e.payload)); break;
    case MsgCode::MsgSelectSum: BuildSum(std::get<MsgSelectSum>(e.payload)); break;
    case MsgCode::MsgSelectPlace:
    case MsgCode::MsgSelectDisField: BuildPlace(std::get<MsgSelectPlace>(e.payload)); break;
    case MsgCode::MsgSelectPosition: BuildPosition(std::get<MsgSelectPosition>(e.payload)); break;
    case MsgCode::MsgSelectCounter: BuildCounter(std::get<MsgSelectCounter>(e.payload)); break;
    case MsgCode::MsgSortCard: {
        const auto& m = std::get<MsgSortCard>(e.payload);
        prompt_ = Prompt{};
        prompt_.kind = Prompt::kSort;
        prompt_.hint = "reorder cards";
        for (size_t i = 0; i < m.cards.size(); ++i)
            prompt_.choices.push_back("card " + std::to_string(i));
        prompt_.max = static_cast<int>(m.cards.size());
        break;
    }
    case MsgCode::MsgAnnounceRace:
    case MsgCode::MsgAnnounceAttrib:
        BuildAnnounce(std::get<MsgAnnounce>(e.payload),
                      e.code == static_cast<uint8_t>(MsgCode::MsgAnnounceRace));
        break;
    case MsgCode::MsgAnnounceCard: BuildAnnounceCard(std::get<MsgAnnounceCard>(e.payload)); break;
    case MsgCode::MsgAnnounceNumber: BuildAnnounceNumber(std::get<MsgAnnounceNumber>(e.payload)); break;
    case MsgCode::MsgSelectBattleCmd: BuildBattleCmd(std::get<MsgSelectBattleCmd>(e.payload)); break;
    case MsgCode::MsgSelectIdleCmd: BuildIdleCmd(std::get<MsgSelectIdleCmd>(e.payload)); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Prompt builders — message -> uniform choice surface
// ---------------------------------------------------------------------------

void Game::BuildYesNo(const proto::MsgSelectYesNo& m) {
    (void)m;
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kYesNo;
    prompt_.hint = "yes or no";
    prompt_.choices = {"yes", "no"};
    prompt_.int_values = {1, 0};
    prompt_.min = prompt_.max = 1;
}

void Game::BuildOption(const proto::MsgSelectOption& m) {
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kOption;
    prompt_.hint = "choose an option";
    for (size_t i = 0; i < m.options.size(); ++i) {
        prompt_.choices.push_back("option " + std::to_string(i) + " (" +
                                  std::to_string(m.options[i]) + ")");
        prompt_.int_values.push_back(static_cast<int32_t>(i));
    }
    prompt_.min = prompt_.max = 1;
}

void Game::BuildChain(const proto::MsgSelectChain& m) {
    if (std::getenv("YGOMCP_DEBUG")) {
        std::fprintf(stderr, "[chain] %zu entries:", m.entries.size());
        for (const auto& en : m.entries)
            std::fprintf(stderr, " flag=%d forced=%d code=%u", en.flag, en.forced, en.code);
        std::fprintf(stderr, "\n");
    }
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kChain;
    prompt_.hint = "activate a chain?";
    prompt_.chain_forced = false;
    for (size_t i = 0; i < m.entries.size(); ++i) {
        const auto& en = m.entries[i];
        if (en.forced)
            prompt_.chain_forced = true;
        std::string label = CardName(cards_, en.code);
        if (en.forced)
            label += " (forced)";
        prompt_.choices.push_back("activate " + label);
        prompt_.sel.push_back(static_cast<uint8_t>(i));
    }
    if (!prompt_.chain_forced)
        prompt_.choices.push_back("decline");
    prompt_.min = prompt_.max = 1;
}

void Game::BuildCardSelect(const proto::MsgSelectCard& m, Prompt::Kind kind) {
    prompt_ = Prompt{};
    prompt_.kind = kind;
    prompt_.hint = "select " + std::to_string(m.min) + ".." + std::to_string(m.max) + " cards";
    prompt_.min = m.min;
    prompt_.max = m.max;
    prompt_.cancelable = m.cancelable != 0;
    prompt_.cards = m.cards;
    for (size_t i = 0; i < m.cards.size(); ++i) {
        prompt_.choices.push_back(CardName(cards_, m.cards[i].code));
        prompt_.sel.push_back(static_cast<uint8_t>(i));
    }
}

void Game::BuildUnselect(const proto::MsgSelectUnselectCard& m) {
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kUnselect;
    prompt_.hint = "choose exactly one card";
    prompt_.min = 1;
    prompt_.max = 1;
    prompt_.cards = m.selectable;
    for (size_t i = 0; i < m.selectable.size(); ++i) {
        prompt_.choices.push_back(CardName(cards_, m.selectable[i].code));
        prompt_.sel.push_back(static_cast<uint8_t>(i));
    }
}

void Game::BuildSum(const proto::MsgSelectSum& m) {
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kSum;
    prompt_.hint = "select cards whose levels sum to " + std::to_string(m.sumval);
    prompt_.min = m.min;
    prompt_.max = m.max;
    prompt_.cards = m.selectable;
    for (size_t i = 0; i < m.selectable.size(); ++i) {
        prompt_.choices.push_back(CardName(cards_, m.selectable[i].code));
        prompt_.sel.push_back(static_cast<uint8_t>(i));
    }
    prompt_.sum_params = m.op_params1;
    int32_t must = 0;
    for (auto v : m.op_params2)
        must += v;
    prompt_.sum_target = m.sumval - must;
    prompt_.sum_lo = std::max(0, static_cast<int>(m.min) - static_cast<int>(m.op_params2.size()));
    prompt_.sum_hi = std::max(0, static_cast<int>(m.max) - static_cast<int>(m.op_params2.size()));
}

void Game::BuildPlace(const proto::MsgSelectPlace& m) {
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kPlace;
    prompt_.hint = "choose a zone";
    prompt_.min = m.count ? 1 : 0;
    prompt_.max = m.count ? static_cast<int>(m.count) : 0;
    prompt_.cancelable = m.count == 0;
    // CAVEAT (engine's select_place): the mask is the FORBIDDEN mask in the
    // PROMPTED player's LOCAL space; the response's player byte must be the
    // prompted player.
    uint32_t allowed = ~m.zone;
    const uint8_t p = m.player;
    for (int i = 0; i < 7; ++i)
        if (allowed & (1u << i)) {
            prompt_.choices.push_back("mzone " + std::to_string(i));
            prompt_.zones.push_back({p, proto::LocMzone, static_cast<uint8_t>(i), 0});
        }
    for (int i = 0; i < 8; ++i)
        if (allowed & (0x100u << i)) {
            prompt_.choices.push_back("szone " + std::to_string(i));
            prompt_.zones.push_back({p, proto::LocSzone, static_cast<uint8_t>(i), 0});
        }
}

void Game::BuildPosition(const proto::MsgSelectPosition& m) {
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kPosition;
    prompt_.hint = "choose a position";
    uint8_t pos = m.positions & 0x0f;
    for (int i = 0; i < 4; ++i) {
        uint8_t bit = static_cast<uint8_t>(1u << i);
        if (pos & bit) {
            static const char* names[4] = {"face-up attack", "face-down attack",
                                           "face-up defense", "face-down defense"};
            prompt_.choices.push_back(names[i]);
            prompt_.int_values.push_back(bit);
        }
    }
    prompt_.min = prompt_.max = 1;
}

void Game::BuildCounter(const proto::MsgSelectCounter& m) {
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kCounter;
    prompt_.hint = "remove " + std::to_string(m.count) + " counters";
    prompt_.min = prompt_.max = 1;
    for (size_t i = 0; i < m.cards.size(); ++i) {
        prompt_.choices.push_back(CardName(cards_, m.cards[i].code) + " (" +
                                  std::to_string(i < m.card_counts.size() ? m.card_counts[i] : 0) +
                                  ")");
        prompt_.sel.push_back(static_cast<uint8_t>(i));
    }
    prompt_.counter_remaining = m.card_counts;
    prompt_.counter_target = m.count;
}

void Game::BuildAnnounce(const proto::MsgAnnounce& m, bool race) {
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kAnnounce;
    prompt_.hint = std::string(race ? "declare a race" : "declare an attribute");
    // For the random driver: list the available bits as choices.
    uint32_t avail = m.available;
    for (int i = 0; i < 32; ++i) {
        if (avail & (1u << i)) {
            prompt_.choices.push_back("value 0x" + [&] {
                char b[16];
                std::snprintf(b, sizeof(b), "%x", 1u << i);
                return std::string(b);
            }());
            prompt_.int_values.push_back(1 << i);
        }
    }
    prompt_.min = prompt_.max = 1;
}

void Game::BuildAnnounceCard(const proto::MsgAnnounceCard& m) {
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kAnnounce;
    prompt_.hint = "declare a card";
    for (size_t i = 0; i < m.opcodes.size(); ++i) {
        prompt_.choices.push_back("opcode " + std::to_string(i) + " (0x" + [&] {
            char b[16];
            std::snprintf(b, sizeof(b), "%x", m.opcodes[i]);
            return std::string(b);
        }() + ")");
        prompt_.int_values.push_back(static_cast<int32_t>(m.opcodes[i]));
    }
    prompt_.min = prompt_.max = 1;
}

void Game::BuildAnnounceNumber(const proto::MsgAnnounceNumber& m) {
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kAnnounce;
    prompt_.hint = "choose a number";
    for (size_t i = 0; i < m.values.size(); ++i) {
        prompt_.choices.push_back(std::to_string(m.values[i]));
        prompt_.int_values.push_back(static_cast<int32_t>(i));  // engine wants the index
    }
    prompt_.min = prompt_.max = 1;
}

void Game::BuildBattleCmd(const proto::MsgSelectBattleCmd& m) {
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kBattleCmd;
    prompt_.hint = "battle phase";
    // ctype: 0=activate, 1=attack, 2=to main phase 2, 3=to end phase.
    for (size_t i = 0; i < m.activatable.size(); ++i) {
        prompt_.choices.push_back("activate " + CardName(cards_, m.activatable[i].code));
        prompt_.int_values.push_back(static_cast<int32_t>(i) << 16);
    }
    for (size_t i = 0; i < m.attackable.size(); ++i) {
        prompt_.choices.push_back("attack with " + CardName(cards_, m.attackable[i].code));
        prompt_.int_values.push_back((static_cast<int32_t>(i) << 16) | 1);
    }
    if (m.to_bp)
        prompt_.choices.push_back("to main phase 2");
    prompt_.int_values.push_back(2);
    if (m.to_ep)
        prompt_.choices.push_back("to end phase");
    prompt_.int_values.push_back(3);
    prompt_.min = prompt_.max = 1;
}

void Game::BuildIdleCmd(const proto::MsgSelectIdleCmd& m) {
    prompt_ = Prompt{};
    prompt_.kind = Prompt::kIdleCmd;
    prompt_.hint = "main phase";
    // CAVEAT (THIS engine's select_idle_command validation — the AUTHORITATIVE
    // code map): 0=summon, 1=sp-summon, 2=reposition, 3=mset, 4=sset,
    // 5=activate, 6=to BP, 7=to EP, 8=shuffle. The gframe client's mapping has
    // 1 and 2 SWAPPED (1=reposition, 2=sp-summon) — using it here RETRYs.
    for (size_t i = 0; i < m.summonable.size(); ++i) {
        prompt_.choices.push_back("summon " + CardName(cards_, m.summonable[i].code));
        prompt_.int_values.push_back(static_cast<int32_t>(i) << 16);
    }
    for (size_t i = 0; i < m.spsummonable.size(); ++i) {
        prompt_.choices.push_back("special summon " + CardName(cards_, m.spsummonable[i].code));
        prompt_.int_values.push_back((static_cast<int32_t>(i) << 16) | 1);
    }
    for (size_t i = 0; i < m.reposable.size(); ++i) {
        prompt_.choices.push_back("reposition " + CardName(cards_, m.reposable[i].code));
        prompt_.int_values.push_back((static_cast<int32_t>(i) << 16) | 2);
    }
    for (size_t i = 0; i < m.msetable.size(); ++i) {
        prompt_.choices.push_back("set monster " + CardName(cards_, m.msetable[i].code));
        prompt_.int_values.push_back((static_cast<int32_t>(i) << 16) | 3);
    }
    for (size_t i = 0; i < m.ssetable.size(); ++i) {
        prompt_.choices.push_back("set spell/trap " + CardName(cards_, m.ssetable[i].code));
        prompt_.int_values.push_back((static_cast<int32_t>(i) << 16) | 4);
    }
    for (size_t i = 0; i < m.activatable.size(); ++i) {
        prompt_.choices.push_back("activate " + CardName(cards_, m.activatable[i].code));
        prompt_.int_values.push_back((static_cast<int32_t>(i) << 16) | 5);
    }
    if (m.to_bp)
        prompt_.choices.push_back("to battle phase");
    prompt_.int_values.push_back(6);
    if (m.to_ep)
        prompt_.choices.push_back("to end phase");
    prompt_.int_values.push_back(7);
    prompt_.min = prompt_.max = 1;
}

// ---------------------------------------------------------------------------
// Response encoding — the protocol knowhow
// ---------------------------------------------------------------------------

bool Game::SumSolve(std::vector<int>& picked) const {
    // subset-sum over the NOT-picked cards to reach the residual target.
    std::vector<int32_t> vals;
    std::vector<uint8_t> pool;  // original indices of the pool
    for (size_t i = 0; i < prompt_.sum_params.size(); ++i) {
        if (std::find(picked.begin(), picked.end(), static_cast<int>(i)) == picked.end()) {
            vals.push_back(prompt_.sum_params[i]);
            pool.push_back(static_cast<uint8_t>(i));
        }
    }
    int32_t have = 0;
    for (int p : picked)
        have += p < static_cast<int>(prompt_.sum_params.size()) ? prompt_.sum_params[p] : 0;
    int32_t target = prompt_.sum_target - have;
    int lo = std::max(0, prompt_.sum_lo - static_cast<int>(picked.size()));
    int hi = std::max(0, prompt_.sum_hi - static_cast<int>(picked.size()));
    std::vector<uint8_t> best;
    std::vector<uint8_t> cur;
    bool found = false;
    std::function<void(int, int32_t)> dfs = [&](int start, int32_t sum) {
        int k = static_cast<int>(cur.size());
        if (k >= lo && k <= hi && sum == target) {
            best = cur;
            found = true;
            return;
        }
        if (found || k >= hi)
            return;
        for (size_t i = start; i < vals.size() && !found; ++i) {
            cur.push_back(pool[i]);
            dfs(static_cast<int>(i) + 1, sum + vals[i]);
            cur.pop_back();
        }
    };
    dfs(0, 0);
    for (auto idx : best)
        picked.push_back(idx);
    return found;
}

bool Game::EncodeResponse(const std::vector<int>& picked, std::vector<uint8_t>& resp,
                          std::string& err) const {
    using namespace proto;
    switch (prompt_.kind) {
    case Prompt::kYesNo:
    case Prompt::kOption:
    case Prompt::kAnnounce:
    case Prompt::kPosition:
    case Prompt::kBattleCmd:
    case Prompt::kIdleCmd: {
        if (picked.empty() || picked[0] < 0 ||
            picked[0] >= static_cast<int>(prompt_.int_values.size())) {
            err = "invalid choice";
            return false;
        }
        resp = ResponseI(prompt_.int_values[picked[0]]);
        return true;
    }
    case Prompt::kChain: {
        // CAVEAT (THIS engine's process_phase_event, verified empirically):
        // the SELECT_CHAIN response is consumed by `--ivalue`, so:
        //   response 0    -> decline (advances the game)
        //   response i+1  -> activate entry i (valid only for i < size-1; the
        //                    last entry retries — engine quirk)
        //   response -1   -> passes select_chain but drives an out-of-range
        //                    index in the caller (the fork's mapping; benign
        //                    in practice but not clean)
        // For the random driver: decline everything. CAVEAT (THIS engine's
        // process_phase_event): the SELECT_CHAIN response is consumed by
        // `--ivalue` — response 0 lands on -1 = the clean decline. -1 itself
        // passes select_chain but drives an out-of-range index in the caller
        // (UB/hang). The gframe client's -1 decline does NOT apply here.
        resp = ResponseI(-1);
        return true;
    }
    case Prompt::kCardSelect: {
        std::vector<uint8_t> idx;
        for (int p : picked) {
            if (p < 0 || p >= static_cast<int>(prompt_.cards.size())) {
                err = "invalid card index";
                return false;
            }
            idx.push_back(static_cast<uint8_t>(p));
        }
        if (static_cast<int>(idx.size()) < prompt_.min ||
            static_cast<int>(idx.size()) > prompt_.max) {
            err = "need " + std::to_string(prompt_.min) + ".." +
                  std::to_string(prompt_.max) + " cards, got " + std::to_string(idx.size());
            return false;
        }
        resp = ResponseCardIndices(idx);
        return true;
    }
    case Prompt::kUnselect: {
        // CAVEAT: the engine hardcodes exactly one card here.
        if (picked.empty()) {
            // Nothing selectable — finish with count 0.
            resp = ResponseCardIndices({});
            return true;
        }
        int p = picked[0];
        if (p < 0 || p >= static_cast<int>(prompt_.cards.size())) {
            err = "invalid card index";
            return false;
        }
        resp = ResponseCardIndices({static_cast<uint8_t>(p)});
        return true;
    }
    case Prompt::kSum: {
        std::vector<int> picks = picked;
        if (!SumSolve(picks)) {
            err = "no valid combination";
            return false;
        }
        std::vector<uint8_t> idx;
        for (int p : picks)
            idx.push_back(static_cast<uint8_t>(p));
        resp = ResponseCardIndices(idx);
        return true;
    }
    case Prompt::kCounter: {
        // Response = [u16 x N] remaining counters per card, no count prefix.
        std::vector<uint16_t> remaining = prompt_.counter_remaining;
        uint16_t to_remove = prompt_.counter_target;
        // Pick cards to remove counters from: all picked cards, then any.
        std::vector<int> order = picked;
        for (size_t i = 0; i < remaining.size() && order.size() < remaining.size(); ++i)
            if (std::find(order.begin(), order.end(), static_cast<int>(i)) == order.end())
                order.push_back(static_cast<int>(i));
        for (int p : order) {
            if (p < 0 || p >= static_cast<int>(remaining.size()) || to_remove == 0)
                continue;
            uint16_t take = std::min(remaining[p], to_remove);
            remaining[p] -= take;
            to_remove -= take;
        }
        resp = ResponseCounters(remaining);
        return true;
    }
    case Prompt::kPlace: {
        // Response = [con,loc,seq] x N with NO count prefix (the engine reads
        // triples until the buffer ends). Empty = cancel.
        std::vector<proto::CardLoc> zones;
        for (int p : picked) {
            if (p < 0 || p >= static_cast<int>(prompt_.zones.size())) {
                err = "invalid zone";
                return false;
            }
            zones.push_back(prompt_.zones[p]);
        }
        resp = ResponseZones(zones);
        return true;
    }
    case Prompt::kSort: {
        std::vector<uint8_t> order(prompt_.choices.size(), 0);
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = static_cast<uint8_t>(i);
        resp = ResponseSortOrder(order);
        return true;
    }
    default:
        err = "no prompt";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// MCP tool handlers
// ---------------------------------------------------------------------------

Json Game::ToolState(const Json&) {
    std::ostringstream os;
    if (!conn_.IsOpen()) {
        os << "(not connected)";
        return os.str();
    }
    os << "LP " << board_.p[0].lp << ":" << board_.p[1].lp << " | turn "
       << board_.turn << " | " << PhaseName(board_.phase) << "\n";
    auto zone_str = [&](const PlayerBoard& p, const char* who) {
        os << who << ": ";
        os << "hand " << p.hand << " | ";
        os << "field [";
        bool first = true;
        for (const auto& c : p.mzone) {
            if (!first)
                os << ", ";
            os << (c.face_up ? CardName(cards_, c.code) : "face-down");
            first = false;
        }
        for (const auto& c : p.szone) {
            if (!first)
                os << ", ";
            os << (c.face_up ? CardName(cards_, c.code) : "set");
            first = false;
        }
        os << "] | ";
        os << "grave " << p.grave << " | removed " << p.removed << " | deck "
           << p.deck << " | extra " << p.extra;
    };
    zone_str(board_.p[0], "you");
    os << "\n";
    zone_str(board_.p[1], "opp");
    os << "\n";
    if (prompt_.kind != Prompt::kNone) {
        if (prompt_.cancelable)
            os << "Option 0~" << prompt_.max << "/" << prompt_.choices.size() << ":\n";
        else
            os << "Option " << prompt_.min << "~" << prompt_.max << "/"
               << prompt_.choices.size() << ":\n";
        for (size_t i = 0; i < prompt_.choices.size(); ++i)
            os << i << ". " << prompt_.choices[i] << "\n";
    } else if (finished_) {
        os << "(game over: " << last_win_ << ")\n";
    }
    return os.str();
}

Json Game::ToolLog(const Json& p) {
    std::ostringstream os;
    // since = 0 (default) -> everything; since = N -> last N lines.
    int since = p.value("since", 0);
    int start = since > 0 ? std::max(0, static_cast<int>(log_.size()) - since) : 0;
    for (size_t i = start; i < log_.size(); ++i)
        os << log_[i] << "\n";
    if (os.str().empty())
        return "(no new events)\n";
    return os.str();
}

Json Game::ToolCard(const Json& p) {
    std::string q = p.value("name", p.value("code", ""));
    if (q.empty())
        return "usage: ygo_card {name: ...} or {code: ...}\n";
    auto results = cards_.Search(q);
    if (results.empty())
        return "not found\n";
    std::ostringstream os;
    for (const auto& c : results) {
        os << c.code << " " << c.name << " | atk " << c.attack << " def " << c.defense
           << " | lv " << c.level << "\n";
        if (!c.text.empty())
            os << "  " << c.text.substr(0, 120) << "\n";
    }
    return os.str();
}

Json Game::ToolChoose(const Json& p) {
    if (prompt_.kind == Prompt::kNone)
        return "no prompt\n";
    std::vector<int> picked;
    if (p.contains("indices") && p["indices"].is_array()) {
        for (const auto& v : p["indices"])
            picked.push_back(v.get<int>());
    } else if (p.contains("id")) {
        picked.push_back(p["id"].get<int>());
    } else if (p.contains("cancel")) {
        if (!prompt_.cancelable)
            return "not cancelable\n";
        picked.clear();
    } else {
        return "usage: ygo_choose {id: N} or {indices: [...]}\n";
    }
    std::vector<uint8_t> resp;
    std::string err;
    if (!EncodeResponse(picked, resp, err))
        return "invalid: " + err + "\n";
    if (std::getenv("YGOMCP_DEBUG")) {
        std::fprintf(stderr, "[choose] kind=%d resp: ", static_cast<int>(prompt_.kind));
        for (auto b : resp)
            std::fprintf(stderr, "%02x ", b);
        std::fprintf(stderr, "\n");
    }
    if (!SendFrame(proto::CtosResponse(resp)))
        return "send failed\n";
    prompt_ = Prompt{};  // cleared after a successful response
    ++prompts_answered_;
    return "ok\n";
}

Json Game::ToolChat(const Json& p) {
    std::string text = p.value("text", "");
    if (text.empty())
        return "usage: ygo_chat {text: ...}\n";
    std::u16string msg;
    for (char c : text)
        msg.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
    if (!SendFrame(proto::CtosChat(msg)))
        return "send failed\n";
    return "ok\n";
}

Json Game::ToolChatlog(const Json&) {
    std::ostringstream os;
    for (const auto& l : chat_)
        os << l << "\n";
    if (os.str().empty())
        return "(no chat)\n";
    return os.str();
}

}  // namespace game
