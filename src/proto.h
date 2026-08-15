// proto.h — wire layer: TCP socket + ygopro framing + STOC/CTOS/MSG codecs.
//
// Fresh implementation; the byte layouts are the ygopro wire contract, verified
// against the reference implementation (ygopro fork client) and the srvpro
// server's own definitions (ygopro-msg-encode). No ygopro/gframe source is
// compiled into this project — this header IS the protocol knowledge, with the
// knowhows we hit in the ygopromcp project as comments.
//
// LAYERING (requirement: each layer separately implementable and testable):
//   proto   <- this file: bytes <-> typed structs. No game logic, no JSON.
//   game    <- consumes/produces proto structs only.
//   mcp     <- pure JSON-RPC + tool registry; game registers tools into it.
// A change in proto's codecs cannot leak into game; game cannot touch bytes.
//
// WIRE FORMAT (the contract, from the reference + srvpro docs):
//   frame : [u16 LE length][identifier u8][body]   (length = 1 + body size)
//   STOC_GAME_MSG body : a SEQUENCE of MSG_* messages. IMPORTANT: MSG_* has NO
//   framing — no length prefix, no per-message header. The boundary of each
//   message derives ONLY from knowing its layout (fixed-size, count-prefixed,
//   or flag-driven). An unknown code therefore means unknown length = stream
//   desync; the layout table below must cover every code the server can emit,
//   and any parse failure is a hard error (never silent garbage).
//   Version gate (PROTOCOL_VERSION, checked at join) is the outer safety net.
//
// CAVEAT (from ygopromcp): srvpro rejects split packets — each frame must be
// written with a single send()/write() call. See Connection::SendFrame.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// Constants (numeric values from the protocol contract)
// ---------------------------------------------------------------------------

namespace proto {

inline constexpr uint16_t PROTOCOL_VERSION = 0x1362;  // client<->server version gate

// ---- CTOS identifiers (client -> server) ----
enum class CtosType : uint8_t {
    Response = 0x01,          // [bytes] game response
    UpdateDeck = 0x02,        // [mainc u32][sidec u32][main+extra codes][side codes]
    HandResult = 0x03,        // [res u8]
    TpResult = 0x04,          // [res u8]
    PlayerInfo = 0x10,        // [name u16 x 20]
    CreateGame = 0x11,        // [HostInfo 20B][name u16x20][pass u16x20]
    JoinGame = 0x12,          // [version u16][pad u16][gameid u32][pass u16x20]
    LeaveGame = 0x13,         // (no data)
    Surrender = 0x14,         // (no data)
    TimeConfirm = 0x15,       // (no data)
    Chat = 0x16,              // [msg u16 x N]
    ExternalAddress = 0x17,   // [real_ip u32][hostname u16 x 256]
    HsToDuelist = 0x20,       // (no data)
    HsToObserver = 0x21,      // (no data)
    HsReady = 0x22,           // (no data)
    HsNotReady = 0x23,        // (no data)
    Kick = 0x24,              // [pos u8]
    HsStart = 0x25,           // (no data)
    RequestField = 0x30,      // (no data)
};

// ---- STOC identifiers (server -> client) ----
enum class StocType : uint8_t {
    GameMsg = 0x01,           // [MSG_* stream]
    ErrorMsg = 0x02,          // [msg u8][pad 3][code u32]  (8B)
    SelectHand = 0x03,        // (no data) -> respond CTOS_HAND_RESULT
    SelectTp = 0x04,          // (no data) -> respond CTOS_TP_RESULT
    HandResult = 0x05,        // [res1 u8][res2 u8]
    TpResult = 0x06,          // (reserved)
    ChangeSide = 0x07,        // (no data) -> respond CTOS_UPDATE_DECK
    WaitingSide = 0x08,       // (no data)
    DeckCount = 0x09,         // [i16 x 6]
    CreateGame = 0x11,        // (reserved)
    JoinGame = 0x12,          // [HostInfo 20B]
    TypeChange = 0x13,        // [type u8]
    LeaveGame = 0x14,         // (reserved)
    DuelStart = 0x15,         // (no data)
    DuelEnd = 0x16,           // (no data)
    Replay = 0x17,            // replay data (observer only; kept raw)
    TimeLimit = 0x18,         // [player u8][pad][left_time u16] (4B)
    Chat = 0x19,              // [player_type u16][msg u16 x N]
    HsPlayerEnter = 0x20,     // [name u16x20][pos u8] (41B)
    HsPlayerChange = 0x21,    // [status u8]
    HsWatchChange = 0x22,     // [watch_count u16]
    TeammateSurrender = 0x23, // (no data)
    FieldFinish = 0x30,       // (no data)
    SrvproRoomlist = 0x31,    // srvpro-specific room list
};

// ---- MSG_* identifiers (payload of STOC_GAME_MSG) ----
enum class MsgCode : uint8_t {
    MsgRetry = 1,
    MsgHint = 2,
    MsgWaiting = 3,
    MsgStart = 4,
    MsgWin = 5,
    MsgUpdateData = 6,
    MsgUpdateCard = 7,
    MsgSelectBattleCmd = 10,
    MsgSelectIdleCmd = 11,
    MsgSelectEffectYn = 12,
    MsgSelectYesNo = 13,
    MsgSelectOption = 14,
    MsgSelectCard = 15,
    MsgSelectChain = 16,
    MsgSelectPlace = 18,
    MsgSelectPosition = 19,
    MsgSelectTribute = 20,
    MsgSelectCounter = 22,
    MsgSelectSum = 23,
    MsgSelectDisField = 24,
    MsgSortCard = 25,
    MsgSelectUnselectCard = 26,
    MsgConfirmDeckTop = 30,
    MsgConfirmCards = 31,
    MsgShuffleDeck = 32,
    MsgShuffleHand = 33,
    MsgRefreshDeck = 34,
    MsgSwapGraveDeck = 35,
    MsgShuffleSetCard = 36,
    MsgReverseDeck = 37,
    MsgDeckTop = 38,
    MsgShuffleExtra = 39,
    MsgNewTurn = 40,
    MsgNewPhase = 41,
    MsgConfirmExtraTop = 42,
    MsgMove = 50,
    MsgPosChange = 53,
    MsgSet = 54,
    MsgSwap = 55,
    MsgFieldDisabled = 56,
    MsgSummoning = 60,
    MsgSummoned = 61,
    MsgSpSummoning = 62,
    MsgSpSummoned = 63,
    MsgFlipSummoning = 64,
    MsgFlipSummoned = 65,
    MsgChaining = 70,
    MsgChained = 71,
    MsgChainSolving = 72,
    MsgChainSolved = 73,
    MsgChainEnd = 74,
    MsgChainNegated = 75,
    MsgChainDisabled = 76,
    MsgCardSelected = 80,
    MsgRandomSelected = 81,
    MsgBecomeTarget = 83,
    MsgDraw = 90,
    MsgDamage = 91,
    MsgRecover = 92,
    MsgEquip = 93,
    MsgLpUpdate = 94,
    MsgUnequip = 95,
    MsgCardTarget = 96,
    MsgCancelTarget = 97,
    MsgPayLpCost = 100,
    MsgAddCounter = 101,
    MsgRemoveCounter = 102,
    MsgAttack = 110,
    MsgBattle = 111,
    MsgAttackDisabled = 112,
    MsgDamageStepStart = 113,
    MsgDamageStepEnd = 114,
    MsgMissedEffect = 120,
    MsgTossCoin = 130,
    MsgTossDice = 131,
    MsgRockPaperScissors = 132,
    MsgHandRes = 133,
    MsgAnnounceRace = 140,
    MsgAnnounceAttrib = 141,
    MsgAnnounceCard = 142,
    MsgAnnounceNumber = 143,
    MsgCardHint = 160,
    MsgTagSwap = 161,
    MsgReloadField = 162,
    MsgPlayerHint = 165,
    MsgMatchKill = 170,
};

// ---- HINT types (MSG_HINT) ----
enum HintType : uint8_t {
    HintEvent = 1,
    HintMessage = 2,
    HintSelectMsg = 3,
    HintOpSelected = 4,
    HintEffect = 5,
    HintRace = 6,
    HintAttrib = 7,
    HintCode = 8,
    HintNumber = 9,
    HintCard = 10,
    HintZone = 11,
};

// ---- ERRMSG / DECKERROR / PLAYERCHANGE codes (STOC_ERROR_MSG) ----
enum ErrMsg : uint8_t {
    ErrJoinError = 0x1,
    ErrDeckError = 0x2,
    ErrSideError = 0x3,
    ErrVersionError = 0x4,
};
enum DeckError : uint32_t {
    DeckErrorLflist = 0x1,
    DeckErrorOcgOnly = 0x2,
    DeckErrorTcgOnly = 0x3,
    DeckErrorUnknownCard = 0x4,
    DeckErrorCardCount = 0x5,
    DeckErrorMainCount = 0x6,
    DeckErrorExtraCount = 0x7,
    DeckErrorSideCount = 0x8,
    DeckErrorNotAvail = 0x9,
};
enum PlayerChange : uint8_t {
    PlayerChangeObserve = 0x8,
    PlayerChangeReady = 0x9,
    PlayerChangeNotReady = 0xa,
    PlayerChangeLeave = 0xb,
};

// ---- zones / phases / positions (from the engine's client contract) ----
enum Location : uint8_t {
    LocDeck = 0x01,
    LocHand = 0x02,
    LocMzone = 0x04,
    LocSzone = 0x08,
    LocGrave = 0x10,
    LocRemoved = 0x20,
    LocExtra = 0x40,
    LocOverlay = 0x80,
};
enum Phase : uint16_t {
    PhaseDraw = 0x01,
    PhaseStandby = 0x02,
    PhaseMain1 = 0x04,
    PhaseBattleStart = 0x08,
    PhaseBattleStep = 0x10,
    PhaseDamage = 0x20,
    PhaseDamageCalc = 0x40,
    PhaseBattle = 0x80,
    PhaseMain2 = 0x100,
    PhaseEnd = 0x200,
};

// ---- QUERY flags (card data in MSG_UPDATE_DATA / MSG_UPDATE_CARD) ----
enum QueryFlag : uint32_t {
    QCode = 0x1,
    QPosition = 0x2,
    QAlias = 0x4,
    QType = 0x8,
    QLevel = 0x10,
    QRank = 0x20,
    QAttribute = 0x40,
    QRace = 0x80,
    QAttack = 0x100,
    QDefense = 0x200,
    QBaseAttack = 0x400,
    QBaseDefense = 0x800,
    QReason = 0x1000,
    QReasonCard = 0x2000,
    QEquipCard = 0x4000,
    QTargetCard = 0x8000,
    QOverlayCard = 0x10000,
    QCounters = 0x20000,
    QOwner = 0x40000,
    QStatus = 0x80000,
    QLscale = 0x200000,
    QRscale = 0x400000,
    QLink = 0x800000,
};

// ---------------------------------------------------------------------------
// Reader / Writer — bounded, error-propagating byte access.
//
// CAVEAT (error-surface ladder): all MSG_* payloads are server-controlled
// byte counts. A malformed or truncated payload must fail the parse with an
// error — never read past the buffer (that class of bug caused desync in the
// GUI client). Every read returns false on underflow; callers must propagate.
// ---------------------------------------------------------------------------

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    size_t remaining() const { return n_ - pos_; }
    bool ok() const { return pos_ <= n_; }
    const uint8_t* data() const { return p_; }
    size_t pos() const { return pos_; }

    bool u8(uint8_t& v) { return take(v); }
    bool u16(uint16_t& v) { return take(v); }
    bool u32(uint32_t& v) { return take(v); }
    bool i32(int32_t& v) { return take(v); }

    bool bytes(size_t n, const uint8_t*& out) {
        if (n_ - pos_ < n) return false;
        out = p_ + pos_;
        pos_ += n;
        return true;
    }
    bool skip(size_t n) {
        if (n_ - pos_ < n) return false;
        pos_ += n;
        return true;
    }
    bool empty() const { return pos_ == n_; }

private:
    template <typename T>
    bool take(T& v) {
        if (n_ - pos_ < sizeof(T)) return false;
        std::memcpy(&v, p_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

class Writer {
public:
    Writer& u8(uint8_t v) { put(v); return *this; }
    Writer& u16(uint16_t v) { put(v); return *this; }
    Writer& u32(uint32_t v) { put(v); return *this; }
    Writer& i32(int32_t v) { put(v); return *this; }
    Writer& bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
        return *this;
    }
    std::vector<uint8_t> take() { return std::move(buf_); }
    const std::vector<uint8_t>& vec() const { return buf_; }

private:
    template <typename T>
    void put(T v) {
        uint8_t b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));
        buf_.insert(buf_.end(), b, b + sizeof(T));
    }
    std::vector<uint8_t> buf_;
};

// UTF-16LE conversion helpers (names/passwords in CTOS/STOC headers).
// ASCII subset for now; wide-string expansion lives in the game layer if ever
// needed (the wire is always UTF-16LE, zero-padded to a fixed array length).
void ToUtf16LE(const std::string& s, uint16_t* out, size_t n);  // truncates+pads
std::string FromUtf16LE(const uint16_t* in, size_t n);          // until first 0

// ---------------------------------------------------------------------------
// Typed message structs (the structs game handles — proto only codecs them)
// ---------------------------------------------------------------------------

struct HostInfo {  // 20 bytes on the wire
    uint32_t lflist = 0;
    uint8_t rule = 0, mode = 0, duel_rule = 0, no_check_deck = 0, no_shuffle_deck = 0;
    uint8_t pad[3] = {0, 0, 0};
    int32_t start_lp = 8000;
    uint8_t start_hand = 5, draw_count = 1;
    uint16_t time_limit = 0;
};
bool DecodeHostInfo(Reader& r, HostInfo& h);
void EncodeHostInfo(Writer& w, const HostInfo& h);

// Card reference in a prompt list: [code u32][controler u8][loc u8][seq u8][ss u8]
struct CardRef {
    uint32_t code = 0;
    uint8_t controler = 0, location = 0, sequence = 0, sub_sequence = 0;
};
// Bare 4-byte card reference (no code): [controler][loc][seq][ss]
struct CardLoc {
    uint8_t controler = 0, location = 0, sequence = 0, sub_sequence = 0;
};

// Query-flag card data (MSG_UPDATE_DATA / MSG_UPDATE_CARD): [flag u32][fields].
// All fields u32 except the 4-byte card refs (equip/target/overlay/reason_card).
struct CardInfo {
    uint32_t flag = 0;
    uint32_t code = 0, position = 0, alias = 0, type = 0, level = 0, rank = 0;
    uint32_t attribute = 0, race = 0, attack = 0, defense = 0;
    uint32_t base_attack = 0, base_defense = 0, reason = 0;
    uint32_t counters = 0, owner = 0, status = 0, lscale = 0, rscale = 0, link = 0;
    CardLoc reason_card, equip_card, target_card;
    std::vector<CardLoc> overlay_cards;
};
// Parses one card-data block from the reader (flag + fields). false = malformed.
bool DecodeCardInfo(Reader& r, CardInfo& c);
void EncodeCardInfo(Writer& w, const CardInfo& c);

// ---- MSG_* typed payloads ----

struct MsgEmpty {};  // messages with no data (retry/summoned/chain_end/...)

struct MsgHint { uint8_t type = 0, player = 0; uint32_t data = 0; };
struct MsgWin { uint8_t player = 0, reason = 0; };
struct MsgStart {
    uint8_t player_type = 0, duel_rule = 0;
    int32_t lp[2] = {8000, 8000};
    uint16_t deck[2] = {0, 0}, extra[2] = {0, 0};
};
struct MsgUpdateData { uint8_t player = 0, location = 0; std::vector<CardInfo> cards; };
struct MsgUpdateCard { uint8_t player = 0, location = 0, sequence = 0; CardInfo card; };

struct EffectRef { uint32_t code = 0; uint8_t controler = 0, location = 0, sequence = 0; int32_t desc = 0; };
struct AttackRef { uint32_t code = 0; uint8_t controler = 0, location = 0, sequence = 0, diratt = 0; };
struct MsgSelectBattleCmd {
    uint8_t player = 0;
    std::vector<EffectRef> activatable;
    std::vector<AttackRef> attackable;
    bool to_bp = false, to_ep = false;  // may enter battle / end phase
};
struct MsgSelectIdleCmd {
    uint8_t player = 0;
    std::vector<CardRef> summonable, spsummonable, reposable, msetable, ssetable;
    std::vector<EffectRef> activatable;
    bool to_bp = false, to_ep = false, shuffle = false;
};
struct MsgSelectEffectYn { uint8_t player = 0; uint32_t code = 0; uint8_t controler = 0, location = 0, sequence = 0, unused = 0; int32_t desc = 0; };
struct MsgSelectYesNo { uint8_t player = 0; int32_t desc = 0; };
struct MsgSelectOption { uint8_t player = 0; std::vector<int32_t> options; };
struct MsgSelectCard { uint8_t player = 0, cancelable = 0, min = 0, max = 0; std::vector<CardRef> cards; };
struct MsgSelectUnselectCard {
    uint8_t player = 0, finishable = 0, cancelable = 0, min = 0, max = 0;
    std::vector<CardRef> selectable, selected;
};
struct ChainEntry { uint8_t flag = 0, forced = 0; uint32_t code = 0; uint8_t controler = 0, location = 0, sequence = 0, sub_sequence = 0; int32_t desc = 0; };
struct MsgSelectChain { uint8_t player = 0, count = 0, specount = 0; int32_t hint0 = 0, hint1 = 0; std::vector<ChainEntry> entries; };
struct MsgSelectPlace { uint8_t player = 0, count = 0; uint32_t zone = 0; };  // zone bitmask
struct MsgSelectPosition { uint8_t player = 0; uint32_t code = 0; uint8_t positions = 0; };
struct MsgSelectTribute { uint8_t player = 0, cancelable = 0, min = 0, max = 0; std::vector<CardRef> cards; };
struct MsgSelectCounter {
    uint8_t player = 0;
    uint16_t type = 0, count = 0;
    std::vector<CardRef> cards;
    std::vector<uint16_t> card_counts;  // per-card counters, parallel to cards
};
struct MsgSelectSum {
    uint8_t mode = 0, player = 0, min = 0, max = 0, must_select_count = 0;
    int32_t sumval = 0;
    std::vector<CardRef> selectable, must_select;
    std::vector<int32_t> op_params1, op_params2;  // per-card opParam, parallel
};
struct MsgSortCard { uint8_t player = 0; std::vector<CardRef> cards; };
struct MsgConfirmCards {
    uint8_t player = 0, skip_panel = 0;
    std::vector<CardRef> cards;  // 7-byte entries (code+c+l+s, no ss)
};
struct MsgConfirmTop { uint8_t player = 0; std::vector<uint32_t> codes; };
struct MsgDeckTop { uint8_t player = 0, sequence = 0; uint32_t code = 0; };
struct MsgNewTurn { uint8_t player = 0; };
struct MsgNewPhase { uint16_t phase = 0; };
struct MsgMove {
    uint32_t code = 0;
    uint8_t prev_controler = 0, prev_location = 0, prev_sequence = 0, prev_position = 0;
    uint8_t cur_controler = 0, cur_location = 0, cur_sequence = 0, cur_position = 0;
    int32_t reason = 0;
};
struct MsgPosChange { uint32_t code = 0; uint8_t controler = 0, location = 0, sequence = 0, prev_position = 0, cur_position = 0; };
struct MsgSet { uint32_t code = 0; uint8_t controler = 0, location = 0, sequence = 0, position = 0; };
struct MsgSwap {
    uint32_t code1 = 0; uint8_t c1 = 0, l1 = 0, s1 = 0, p1 = 0;
    uint32_t code2 = 0; uint8_t c2 = 0, l2 = 0, s2 = 0, p2 = 0;
};
struct MsgFieldDisabled { uint32_t disabled = 0; };
struct MsgSummoning { uint32_t code = 0; uint8_t controler = 0, location = 0, sequence = 0, position = 0; };
struct MsgChaining {
    uint32_t code = 0;
    uint8_t prev_controler = 0, prev_location = 0, prev_sequence = 0, sub_sequence = 0;
    uint8_t cur_controler = 0, cur_location = 0, cur_sequence = 0;
    int32_t desc = 0;
    uint8_t chain_count = 0;
};
struct MsgChainCount { uint8_t chain_count = 0; };
struct MsgRandomSelected { uint8_t player = 0; std::vector<CardLoc> cards; };
struct MsgBecomeTarget { std::vector<CardLoc> cards; };
struct MsgDraw { uint8_t player = 0; std::vector<uint32_t> codes; };
struct MsgLp { uint8_t player = 0; int32_t value = 0; };
struct MsgEquip { CardLoc equip, target; };
struct MsgTargetPair { CardLoc source, target; };
struct MsgCounter { uint16_t type = 0; uint8_t controler = 0, location = 0, sequence = 0; uint16_t count = 0; };
struct MsgAttack { CardLoc attacker, target; };
struct MsgBattle {
    CardLoc attacker, target;
    int32_t a_atk = 0, a_def = 0, d_atk = 0, d_def = 0;
    uint8_t a_chain = 0, d_chain = 0;
};
struct MsgMissedEffect { uint32_t desc = 0, code = 0; };
struct MsgToss { uint8_t player = 0; std::vector<uint8_t> results; };
struct MsgRps { uint8_t player = 0; };
struct MsgHandRes { uint8_t result = 0; };
struct MsgAnnounce { uint8_t player = 0, count = 0; uint32_t available = 0; };
struct MsgAnnounceCard { uint8_t player = 0; std::vector<uint32_t> opcodes; };
struct MsgAnnounceNumber { uint8_t player = 0; std::vector<int32_t> values; };
struct MsgCardHint { CardLoc card; uint8_t type = 0; int32_t value = 0; };
struct MsgPlayerHint { uint8_t player = 0, type = 0; int32_t value = 0; };
struct MsgMatchKill { uint32_t code = 0; };
struct MsgShuffleSet { uint8_t location = 0; std::vector<CardLoc> cards, shuffled; };
struct MsgTagSwap {
    uint8_t player = 0;
    uint8_t counts[4] = {0, 0, 0, 0};
    int32_t topcode = 0;
    std::array<std::vector<uint32_t>, 4> lists;
};
struct MsgReloadField {};  // debug-only; see note in proto.cpp

struct MsgUnknown { std::vector<uint8_t> body; };

using MsgPayload = std::variant<
    MsgEmpty, MsgUnknown,
    MsgHint, MsgWin, MsgStart, MsgUpdateData, MsgUpdateCard,
    MsgSelectBattleCmd, MsgSelectIdleCmd, MsgSelectEffectYn, MsgSelectYesNo,
    MsgSelectOption, MsgSelectCard, MsgSelectUnselectCard, MsgSelectChain,
    MsgSelectPlace, MsgSelectPosition, MsgSelectTribute, MsgSelectCounter,
    MsgSelectSum, MsgSortCard, MsgConfirmCards, MsgDeckTop, MsgNewTurn,
    MsgNewPhase, MsgMove, MsgPosChange, MsgSet, MsgSwap, MsgFieldDisabled,
    MsgSummoning, MsgChaining, MsgChainCount, MsgRandomSelected,
    MsgBecomeTarget, MsgDraw, MsgLp, MsgEquip, MsgTargetPair, MsgCounter,
    MsgAttack, MsgBattle, MsgMissedEffect, MsgToss, MsgRps, MsgHandRes,
    MsgAnnounce, MsgAnnounceCard, MsgAnnounceNumber, MsgCardHint,
    MsgPlayerHint, MsgMatchKill, MsgShuffleSet, MsgTagSwap, MsgReloadField,
    MsgConfirmTop>;

// One MSG_* in the game-message stream. `raw` = code byte + payload, verbatim
// (for --dump capture, fixture replay, and round-trip tests).
struct MsgEvent {
    uint8_t code = 0;
    std::vector<uint8_t> raw;
    MsgPayload payload;
};

// ---- STOC packet ----
struct StocErrorMsg { uint8_t msg = 0; uint32_t code = 0; };
struct StocHandResult { uint8_t res1 = 0, res2 = 0; };
struct StocDeckCount { int16_t counts[6] = {0, 0, 0, 0, 0, 0}; };
struct StocJoinGame { HostInfo info; };
struct StocTypeChange { uint8_t type = 0; };
struct StocTimeLimit { uint8_t player = 0; uint16_t left_time = 0; };
struct StocChat { uint16_t player_type = 0; std::u16string msg; };
struct StocHsPlayerEnter { std::u16string name; uint8_t pos = 0; };
struct StocHsPlayerChange { uint8_t status = 0; };
struct StocHsWatchChange { uint16_t watch_count = 0; };
struct SrvproRoomInfo {
    std::string roomname;  // 64B UTF-8
    uint8_t room_status = 0;
    int8_t duel_count = 0, turn_count = 0;
    std::string p1, p2;    // 128B UTF-8 each
    int8_t p1_score = 0, p2_score = 0;
    int32_t p1_lp = 0, p2_lp = 0;
};
struct StocSrvproRoomlist { uint16_t count = 0; std::vector<SrvproRoomInfo> rooms; };

using StocBody = std::variant<
    std::monostate,               // no-data packets
    std::vector<MsgEvent>,        // StocGameMsg
    StocErrorMsg, StocHandResult, StocDeckCount, StocJoinGame, StocTypeChange,
    StocTimeLimit, StocChat, StocHsPlayerEnter, StocHsPlayerChange,
    StocHsWatchChange, StocSrvproRoomlist, std::vector<uint8_t>>;  // Replay(raw)

struct StocPacket {
    StocType type;
    std::vector<uint8_t> raw;  // identifier + body, verbatim
    StocBody body;
};

// ---------------------------------------------------------------------------
// Codec entry points
// ---------------------------------------------------------------------------

// Decodes one STOC payload ([identifier][body], i.e. everything after the
// 2-byte frame length). Returns false + error on malformed input. The MSG_*
// stream inside StocGameMsg is decoded with the full layout table; any parse
// failure is a hard error (see header comment).
bool DecodeStoc(const uint8_t* payload, size_t len, StocPacket& out, std::string& err);
bool EncodeStoc(const StocPacket& pkt, std::vector<uint8_t>& frame, std::string& err);

// MSG_* stream decode/encode (the layout table).
bool DecodeMsgs(Reader& r, std::vector<MsgEvent>& out, std::string& err);
bool EncodeMsgs(const std::vector<MsgEvent>& msgs, Writer& w);

// CTOS builders: each returns the complete frame ([u16 len][identifier][body]).
std::vector<uint8_t> CtosPlayerInfo(const std::string& name);
std::vector<uint8_t> CtosJoinGame(const std::string& pass, uint32_t gameid = 0);
std::vector<uint8_t> CtosCreateGame(const HostInfo& info, const std::string& name, const std::string& pass);
std::vector<uint8_t> CtosUpdateDeck(const std::vector<uint32_t>& main_extra, const std::vector<uint32_t>& side);
std::vector<uint8_t> CtosResponse(const std::vector<uint8_t>& data);
std::vector<uint8_t> CtosChat(const std::u16string& msg);
std::vector<uint8_t> CtosNoData(CtosType t);
std::vector<uint8_t> CtosHandResult(uint8_t res);
std::vector<uint8_t> CtosTpResult(uint8_t res);
std::vector<uint8_t> CtosKick(uint8_t pos);

// CTOS_RESPONSE body builders — the response byte formats (wire knowledge).
// CAVEATS (learned the hard way in ygopromcp; tests guard these):
//  - card-select responses are INDICES into the selectable list: [count u8][idx...]
//  - SELECT_UNSELECT_CARD must respond with exactly 1 card when it demands one
//    (the engine hardcodes min=max=1 for hand/field unselect prompts)
//  - PLACE/DISFIELD respond [con,loc,seq] triples with NO count prefix
//  - COUNTER responds [u16 x N] remaining counts with NO count prefix
//  - battle/idle commands pack (index << 16) | action into one i32
inline std::vector<uint8_t> ResponseI(int32_t v) {
    Writer w;
    w.i32(v);
    return w.take();
}
inline std::vector<uint8_t> ResponseCardIndices(const std::vector<uint8_t>& indices) {
    Writer w;
    w.u8(static_cast<uint8_t>(indices.size()));
    for (auto i : indices)
        w.u8(i);
    return w.take();
}
inline std::vector<uint8_t> ResponseZones(const std::vector<CardLoc>& zones) {
    Writer w;
    for (const auto& z : zones)
        w.u8(z.controler).u8(z.location).u8(z.sequence);
    return w.take();
}
inline std::vector<uint8_t> ResponseCounters(const std::vector<uint16_t>& remaining) {
    Writer w;
    for (auto r : remaining)
        w.u16(r);
    return w.take();
}
inline std::vector<uint8_t> ResponseSortOrder(const std::vector<uint8_t>& order) {
    Writer w;
    for (auto o : order)
        w.u8(o);
    return w.take();
}

// ---------------------------------------------------------------------------
// Connection — POSIX socket + frame reassembly.
//
// Merged into proto (per design): framing is a 3-line loop around recv(), a
// separate net layer would be ceremony. CAVEAT (srvpro quirk, from ygopromcp):
// each frame must be sent in ONE write() — the server rejects split packets.
// ---------------------------------------------------------------------------

class Connection {
public:
    Connection() = default;
    ~Connection() { Close(); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool Connect(const std::string& host, uint16_t port, std::string& err);
    int fd() const { return fd_; }
    bool IsOpen() const { return fd_ >= 0; }

    // Sends one complete frame with a single write() (the srvpro quirk).
    bool SendFrame(const std::vector<uint8_t>& frame, std::string& err);
    // Reads available data, reassembles frames, decodes each into `out`.
    // `raw_frames` receives every reassembled frame (including one that fails
    // to decode) so the caller can dump the wire bytes for diagnostics; `out`
    // gets the successfully decoded packets — on a decode failure the earlier
    // packets in the batch are still returned (the caller decides whether to
    // act on them; the failed frame's raw bytes are in raw_frames).
    // Returns false on socket error or protocol decode failure (err set).
    bool ReadPackets(std::vector<StocPacket>& out, std::vector<std::vector<uint8_t>>& raw_frames,
                     std::string& err);
    void Close();

private:
    int fd_ = -1;
    std::vector<uint8_t> rbuf_;  // partial-frame reassembly buffer
};

}  // namespace proto
