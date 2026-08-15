// game.h — business logic layer (M3+M4).
//
// Consumes proto::StocPacket/MsgEvent structs; produces proto CTOS frames.
// Owns the connection lifecycle, the MSG_* state machine, the prompt surface,
// the response encoding (all the protocol knowhows from the wire layer work),
// a client-side board model, the card database, and the ygo_* MCP tool
// handlers (registered into mcp). Pure logic: no JSON in here beyond the tool
// params/results, no direct socket access beyond proto::Connection.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "proto.h"

namespace game {

// ---------------------------------------------------------------------------
// Card database (cards.cdb, sqlite3). Data only — reused, not reimplemented.
// ---------------------------------------------------------------------------

struct Card {
    uint32_t code = 0;
    std::string name;
    std::string text;
    uint32_t type = 0, attribute = 0, race = 0, level = 0, rank = 0;
    int32_t attack = 0, defense = 0;
    uint32_t lscale = 0, rscale = 0, link = 0;
};

class CardDb {
public:
    ~CardDb();
    CardDb(const CardDb&) = delete;
    CardDb& operator=(const CardDb&) = delete;
    CardDb() = default;
    bool Open(const std::string& path, std::string& err);
    bool Get(uint32_t code, Card& out) const;
    // name search (exact, then prefix, then substring); up to `limit` results
    std::vector<Card> Search(const std::string& q, size_t limit = 20) const;
    const std::string& Error() const { return err_; }

private:
    void* db_ = nullptr;  // sqlite3*
    std::string err_;
    mutable std::map<uint32_t, Card> cache_;
};

// ---------------------------------------------------------------------------
// Board model — a client-side view of both fields. Moderate detail: card
// counts per location + face-up field cards; the full rendering is a display
// concern of ygo_state.
// ---------------------------------------------------------------------------

struct BoardCard {
    uint32_t code = 0;
    uint8_t position = 0;   // POS_* from the engine
    bool face_up = false;
};

struct PlayerBoard {
    int32_t lp = 8000;
    int deck = 0, extra = 0, hand = 0, grave = 0, removed = 0;
    std::vector<BoardCard> mzone;  // seq 0..6
    std::vector<BoardCard> szone;  // seq 0..7
    std::vector<BoardCard> grave_list, removed_list, extra_list;
};

struct Board {
    int turn = 1;
    uint16_t phase = 0;
    PlayerBoard p[2];
};

// ---------------------------------------------------------------------------
// The prompt surface: every prompt becomes a uniform list of numbered choices;
// ygo_choose picks by index/indices; the game encodes the response.
// ---------------------------------------------------------------------------

struct Prompt {
    // Kind discriminates the response encoding (the engine's expectations).
    enum Kind {
        kNone,
        kYesNo,        // i32 0/1
        kOption,       // i32 index
        kChain,        // i32 index or -1 (decline); forced entries must be taken
        kPosition,     // i32 position bit
        kAnnounce,     // i32 value
        kCardSelect,   // [count u8][indices]; count within [min,max]
        kUnselect,     // exactly ONE index (engine hardcode min=max=1)
        kSum,          // indices summing to sumval (auto-completed by the game)
        kCounter,      // [u16 x N] remaining counts
        kPlace,        // [con,loc,seq] x N (no count prefix)
        kSort,         // positions per card (no prefix)
        kBattleCmd,    // (sel << 16) | ctype; engine-specific codes
        kIdleCmd,      // (sel << 16) | ctype
    };

    Kind kind = kNone;
    std::string hint;                 // prompt heading (e.g. "select a card")
    int min = 0, max = 0;             // selection count bounds
    bool cancelable = false;
    std::vector<std::string> choices; // displayed options (numbered)
    std::vector<uint8_t> sel;         // per-choice: card index in the message list
    std::vector<uint16_t> counter_remaining;  // per-card remaining counters
    uint16_t counter_target = 0;              // total counters to remove
    std::vector<proto::CardLoc> zones;        // per-choice: allowed zone
    std::vector<int32_t> int_values;  // per-choice: encoded value (announce/position/battle/idle)
    // sum solver inputs (auto-complete)
    std::vector<int32_t> sum_params;  // per selectable card
    int32_t sum_target = 0;
    int sum_lo = 0, sum_hi = 0;
    bool chain_forced = false;        // any entry forced (must not decline)
    std::vector<proto::CardRef> cards;  // the message card list (for card-select)
};

// ---------------------------------------------------------------------------
// Game — lifecycle + state machine + tool handlers.
// ---------------------------------------------------------------------------

class Game {
public:
    explicit Game(proto::Connection& conn);

    // ----- lifecycle (driven by main's poll loop) -----
    bool Start(const std::string& host, uint16_t port, const std::string& room,
               const std::string& name, const std::string& deckfile, std::string& err);
    bool OpenCardDb(const std::string& path, std::string& err);
    // Feed one decoded STOC packet (network thread / poll loop).
    void OnPacket(const proto::StocPacket& pkt);
    bool ReadAndDispatch(std::string& err);  // read + dispatch all available
    bool IsConnected() const;

    // ----- MCP tool handlers (registered into mcp) -----
    nlohmann::json ToolState(const nlohmann::json& p);
    nlohmann::json ToolLog(const nlohmann::json& p);
    nlohmann::json ToolCard(const nlohmann::json& p);
    nlohmann::json ToolChoose(const nlohmann::json& p);
    nlohmann::json ToolChat(const nlohmann::json& p);
    nlohmann::json ToolChatlog(const nlohmann::json& p);

    // ----- status (for the e2e test / debugging) -----
    int Turns() const { return board_.turn; }
    int PromptsAnswered() const { return prompts_answered_; }
    int Retries() const { return retries_; }
    bool Finished() const { return finished_; }
    std::string LastWin() const { return last_win_; }

private:
    // lifecycle helpers
    bool SendFrame(const std::vector<uint8_t>& frame);
    bool SendDeckAndReady();
    void HandlePacket(const proto::StocPacket& pkt);
    void HandleMsg(const proto::MsgEvent& e);
    void OnPrompt(const proto::MsgEvent& e);
    bool RespondPrompt(std::string& err);

    // message interpretation
    void OnMove(const proto::MsgMove& m);
    void OnUpdateData(const proto::MsgUpdateData& m);
    void OnUpdateCard(const proto::MsgUpdateCard& m);
    void OnDraw(const proto::MsgDraw& m);
    void LogNarration(const std::string& line);

    // prompt builders (message -> Prompt surface)
    void BuildYesNo(const proto::MsgSelectYesNo& m);
    void BuildOption(const proto::MsgSelectOption& m);
    void BuildChain(const proto::MsgSelectChain& m);
    void BuildCardSelect(const proto::MsgSelectCard& m, Prompt::Kind kind);
    void BuildUnselect(const proto::MsgSelectUnselectCard& m);
    void BuildSum(const proto::MsgSelectSum& m);
    void BuildPlace(const proto::MsgSelectPlace& m);
    void BuildPosition(const proto::MsgSelectPosition& m);
    void BuildCounter(const proto::MsgSelectCounter& m);
    void BuildAnnounce(const proto::MsgAnnounce& m, bool race);
    void BuildAnnounceCard(const proto::MsgAnnounceCard& m);
    void BuildAnnounceNumber(const proto::MsgAnnounceNumber& m);
    void BuildBattleCmd(const proto::MsgSelectBattleCmd& m);
    void BuildIdleCmd(const proto::MsgSelectIdleCmd& m);

    // response encoding (the knowhow lives here; see each function's comment)
    bool EncodeResponse(const std::vector<int>& picked, std::vector<uint8_t>& resp,
                        std::string& err) const;
    bool SumSolve(std::vector<int>& picked) const;

    proto::Connection& conn_;
    CardDb cards_;
    Board board_;
    Prompt prompt_;
    std::vector<std::string> log_;      // narration lines
    std::vector<std::string> chat_;     // chat lines
    std::vector<proto::MsgEvent> pending_msgs_;  // batch staging (two-pass)

    std::string name_, room_, deckfile_;
    std::vector<uint32_t> deck_main_extra_, deck_side_;
    bool joined_ = false, started_ = false;
    int my_type_ = -1;
    bool ready_[4] = {false, false, false, false};
    int prompts_answered_ = 0, retries_ = 0;
    bool finished_ = false;
    std::string last_win_;
    bool last_prompt_sent_ = false;
};

}  // namespace game
