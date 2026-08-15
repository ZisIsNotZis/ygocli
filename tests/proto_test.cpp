// proto_test.cpp — wire-layer tests.
//
// The core guarantee: for every MSG code the server can emit, a hand-built byte
// vector (independent of the codec's own encode path) must decode to the right
// typed struct AND re-encode byte-identically. That is the "layout table is
// exact" proof — a wrong length here desyncs the whole stream.

#include "proto.h"

#include <string>
#include <vector>

#include "test.h"

using namespace proto;

namespace {

// Builds the raw bytes of one MSG_* message (code byte + payload).
using MsgBuilder = std::vector<uint8_t> (*)();

uint8_t LE16(uint16_t v) { return static_cast<uint8_t>(v & 0xff); }
uint8_t LE32(uint32_t v) { return static_cast<uint8_t>(v & 0xff); }
uint8_t LE32B(uint32_t v) { return static_cast<uint8_t>((v >> 8) & 0xff); }
uint8_t LE32C(uint32_t v) { return static_cast<uint8_t>((v >> 16) & 0xff); }
uint8_t LE32D(uint32_t v) { return static_cast<uint8_t>((v >> 24) & 0xff); }

void P(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(LE32(x));
    v.push_back(LE32B(x));
    v.push_back(LE32C(x));
    v.push_back(LE32D(x));
}

// ---- hand-built message builders (independent of the codec's encode path) ----

std::vector<uint8_t> BSelectCard() {  // 2 selectable cards, min 1 max 2
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgSelectCard), 0, 1, 1, 2, 2};
    P(v, 1111); v.insert(v.end(), {1, LocMzone, 3, 0});
    P(v, 2222); v.insert(v.end(), {0, LocHand, 4, 0});
    return v;
}
std::vector<uint8_t> BUnselectCard() {  // finishable+cancelable, 1 selectable + 1 selected
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgSelectUnselectCard), 0, 1, 1, 1, 1, 1};
    P(v, 3333); v.insert(v.end(), {0, LocHand, 2, 0});
    v.push_back(1);
    P(v, 4444); v.insert(v.end(), {0, LocMzone, 0, 0});
    return v;
}
std::vector<uint8_t> BSum() {  // mode 0, sumval 5, 3 selectable with opParams
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgSelectSum), 0, 0};
    P(v, 5); v.insert(v.end(), {1, 2, 0});  // sumval i32, min 1, max 2, must 0
    v.push_back(3);
    for (int i = 0; i < 3; ++i) {
        P(v, 1000 + i);
        v.insert(v.end(), {0, LocHand, static_cast<uint8_t>(i), 0});
        P(v, static_cast<uint32_t>(i + 1));  // opParam 1,2,3
    }
    v.push_back(0);  // must-select list empty
    return v;
}
std::vector<uint8_t> BBattle() {
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgBattle), 0, LocMzone, 0, 0};
    P(v, 2500); P(v, 2000); v.push_back(0);
    v.insert(v.end(), {1, LocMzone, 1, 0});
    P(v, 1800); P(v, 1500); v.push_back(0);
    return v;
}
std::vector<uint8_t> BMove() {
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgMove)};
    P(v, 9999);
    v.insert(v.end(), {0, LocHand, 2, 0});
    v.insert(v.end(), {0, LocMzone, 5, 1});
    P(v, 0x8000);  // REASON_EFFECT
    return v;
}
std::vector<uint8_t> BChain() {
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgSelectChain), 0, 1, 0};
    P(v, 0); P(v, 0);  // hint0, hint1
    v.insert(v.end(), {0, 1});  // flag, forced
    P(v, 7777); v.insert(v.end(), {0, LocGrave, 1, 0});
    P(v, 12345);  // desc
    return v;
}
std::vector<uint8_t> BIdleCmd() {
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgSelectIdleCmd), 0};
    auto card_list = [&](const std::vector<uint32_t>& codes) {
        v.push_back(static_cast<uint8_t>(codes.size()));
        for (auto c : codes) {
            P(v, c);
            v.insert(v.end(), {0, LocHand, 1});  // 7-byte entries (no ss)
        }
    };
    card_list({111});        // summonable
    card_list({});           // spsummonable
    card_list({});           // reposable
    card_list({});           // msetable
    card_list({});           // ssetable
    v.push_back(0);          // activatable
    v.insert(v.end(), {1, 1, 0});  // to_bp, to_ep, shuffle
    return v;
}
std::vector<uint8_t> BDraw() {
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgDraw), 0, 2};
    P(v, 111); P(v, 222);
    return v;
}
std::vector<uint8_t> BWin() {
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgWin), 0, 1};
    return v;
}
std::vector<uint8_t> BPlace() {
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgSelectPlace), 0, 1};
    P(v, 0x7f);  // all 7 main monster zones
    return v;
}
std::vector<uint8_t> BChaining() {
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgChaining)};
    P(v, 5555);
    v.insert(v.end(), {0, 0, 0, 0});
    v.insert(v.end(), {0, LocSzone, 6});
    P(v, 67890);
    v.push_back(1);
    return v;
}
std::vector<uint8_t> BUpdateCard() {
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgUpdateCard), 0, LocMzone, 0};
    uint32_t body_len = 4 + 4 + 4;  // flag + code + type
    P(v, 4 + body_len);
    P(v, static_cast<uint32_t>(QCode | QType));
    P(v, 8888);
    P(v, 0x21);  // TYPE_MONSTER
    return v;
}
std::vector<uint8_t> BHint() {
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgHint), HintMessage, 1};
    P(v, 1407);
    return v;
}
std::vector<uint8_t> BAnnounceCard() {
    std::vector<uint8_t> v{static_cast<uint8_t>(MsgCode::MsgAnnounceCard), 0, 2};
    P(v, 1001); P(v, 1002);
    return v;
}

// ---- round-trip harness: builder bytes -> decode -> encode -> same bytes ----
void CheckRoundTrip(MsgBuilder b) {
    std::vector<uint8_t> raw = b();
    Reader r(raw.data(), raw.size());
    std::vector<MsgEvent> msgs;
    std::string err;
    CHECK_MSG(DecodeMsgs(r, msgs, err), "decode: " + err);
    CHECK_EQ(msgs.size(), size_t(1));
    if (msgs.size() != 1)
        return;
    CHECK_EQ(msgs[0].code, raw[0]);
    Writer w;
    CHECK(EncodeMsgs(msgs, w));
    CHECK(w.vec() == raw);
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(Reader_bounds) {
    uint8_t b[3] = {1, 2, 3};
    Reader r(b, 3);
    uint8_t v = 0;
    uint16_t v16 = 0;
    uint32_t v32 = 0;
    CHECK(r.u8(v) && v == 1);
    CHECK(r.u16(v16) && v16 == 0x0302);
    CHECK(!r.u32(v32));  // underflow -> false
    CHECK(!r.u8(v));
    CHECK(r.empty());
}

TEST(Writer_roundtrip) {
    Writer w;
    w.u8(1).u16(0x1234).u32(0xdeadbeef).i32(-1);
    auto v = w.take();
    CHECK_EQ(v.size(), size_t(1 + 2 + 4 + 4));
    Reader r(v.data(), v.size());
    uint8_t a; uint16_t b; uint32_t c; int32_t d;
    CHECK(r.u8(a) && r.u16(b) && r.u32(c) && r.i32(d));
    CHECK_EQ(a, uint8_t(1));
    CHECK_EQ(b, uint16_t(0x1234));
    CHECK_EQ(c, uint32_t(0xdeadbeef));
    CHECK_EQ(d, int32_t(-1));
}

TEST(CtosPlayerInfo_frame) {
    auto f = CtosPlayerInfo("AB");
    // [len u16 = 1+40][id 0x10][name u16x20]
    CHECK_EQ(f.size(), size_t(2 + 41));
    CHECK_EQ(f[0], uint8_t(41));
    CHECK_EQ(f[1], uint8_t(0));
    CHECK_EQ(f[2], uint8_t(0x10));
    CHECK_EQ(f[3], uint8_t('A'));
    CHECK_EQ(f[4], uint8_t(0));
    CHECK_EQ(f[5], uint8_t('B'));
    CHECK_EQ(f[6], uint8_t(0));
    CHECK_EQ(f[7], uint8_t(0));  // pad
}

TEST(CtosJoinGame_frame) {
    auto f = CtosJoinGame("room");
    CHECK_EQ(f.size(), size_t(2 + 1 + 48));
    CHECK_EQ(f[2], uint8_t(0x12));
    CHECK_EQ(f[3], uint8_t(0x62));  // version 0x1362 LE
    CHECK_EQ(f[4], uint8_t(0x13));
    // version u16 at 3..4, pad u16 at 5..6, gameid u32 at 7..10, pass at 11
    CHECK_EQ(f[11], uint8_t('r'));
    CHECK_EQ(f[13], uint8_t('o'));
}

TEST(CtosUpdateDeck_frame) {
    std::vector<uint32_t> me{111, 222};
    std::vector<uint32_t> side{333};
    auto f = CtosUpdateDeck(me, side);
    // payload: mainc u32=2, sidec u32=1, codes
    CHECK_EQ(f[2], uint8_t(0x02));
    CHECK_EQ(f[3], uint8_t(2));   // mainc LE
    CHECK_EQ(f[7], uint8_t(1));   // sidec LE
    CHECK_EQ(f[11], uint8_t(111));  // first main code
    CHECK_EQ(f[15], uint8_t(222));
    CHECK_EQ(f[19], uint8_t(333));
}

TEST(Msg_roundtrip_select_card) { CheckRoundTrip(BSelectCard); }
TEST(Msg_roundtrip_unselect_card) { CheckRoundTrip(BUnselectCard); }
TEST(Msg_roundtrip_sum) { CheckRoundTrip(BSum); }
TEST(Msg_roundtrip_battle) { CheckRoundTrip(BBattle); }
TEST(Msg_roundtrip_move) { CheckRoundTrip(BMove); }
TEST(Msg_roundtrip_chain) { CheckRoundTrip(BChain); }
TEST(Msg_roundtrip_idlecmd) { CheckRoundTrip(BIdleCmd); }
TEST(Msg_roundtrip_draw) { CheckRoundTrip(BDraw); }
TEST(Msg_roundtrip_place) { CheckRoundTrip(BPlace); }
TEST(Msg_roundtrip_chaining) { CheckRoundTrip(BChaining); }
TEST(Msg_roundtrip_update_card) { CheckRoundTrip(BUpdateCard); }
TEST(Msg_roundtrip_hint) { CheckRoundTrip(BHint); }
TEST(Msg_roundtrip_announce_card) { CheckRoundTrip(BAnnounceCard); }

TEST(Msg_select_card_fields) {
    std::vector<uint8_t> raw = BSelectCard();
    Reader r(raw.data(), raw.size());
    std::vector<MsgEvent> msgs;
    std::string err;
    CHECK(DecodeMsgs(r, msgs, err));
    const auto& m = std::get<MsgSelectCard>(msgs[0].payload);
    CHECK_EQ(m.player, uint8_t(0));
    CHECK_EQ(m.cancelable, uint8_t(1));
    CHECK_EQ(m.min, uint8_t(1));
    CHECK_EQ(m.max, uint8_t(2));
    CHECK_EQ(m.cards.size(), size_t(2));
    CHECK_EQ(m.cards[0].code, uint32_t(1111));
    CHECK_EQ(m.cards[0].location, uint8_t(LocMzone));
    CHECK_EQ(m.cards[1].code, uint32_t(2222));
    CHECK_EQ(m.cards[1].location, uint8_t(LocHand));
}

TEST(Msg_sum_fields) {
    std::vector<uint8_t> raw = BSum();
    Reader r(raw.data(), raw.size());
    std::vector<MsgEvent> msgs;
    std::string err;
    CHECK(DecodeMsgs(r, msgs, err));
    const auto& m = std::get<MsgSelectSum>(msgs[0].payload);
    CHECK_EQ(m.sumval, int32_t(5));
    CHECK_EQ(m.min, uint8_t(1));
    CHECK_EQ(m.max, uint8_t(2));
    CHECK_EQ(m.selectable.size(), size_t(3));
    CHECK_EQ(m.op_params1[0], int32_t(1));
    CHECK_EQ(m.op_params1[2], int32_t(3));
    CHECK_EQ(m.must_select.size(), size_t(0));
}

TEST(Msg_battle_fields) {
    std::vector<uint8_t> raw = BBattle();
    Reader r(raw.data(), raw.size());
    std::vector<MsgEvent> msgs;
    std::string err;
    CHECK(DecodeMsgs(r, msgs, err));
    const auto& m = std::get<MsgBattle>(msgs[0].payload);
    CHECK_EQ(m.attacker.location, uint8_t(LocMzone));
    CHECK_EQ(m.a_atk, int32_t(2500));
    CHECK_EQ(m.a_def, int32_t(2000));
    CHECK_EQ(m.d_atk, int32_t(1800));
    CHECK_EQ(m.d_def, int32_t(1500));
    CHECK_EQ(raw.size(), size_t(1 + 26));  // 26-byte fixed layout
}

TEST(Msg_multi_stream) {
    std::vector<uint8_t> raw;
    auto h = BHint();
    auto w1 = BWin();
    auto d = BDraw();
    raw.insert(raw.end(), h.begin(), h.end());
    raw.insert(raw.end(), w1.begin(), w1.end());
    raw.insert(raw.end(), d.begin(), d.end());
    Reader r(raw.data(), raw.size());
    std::vector<MsgEvent> msgs;
    std::string err;
    CHECK(DecodeMsgs(r, msgs, err));
    CHECK_EQ(msgs.size(), size_t(3));
    CHECK_EQ(msgs[0].code, uint8_t(MsgCode::MsgHint));
    CHECK_EQ(msgs[1].code, uint8_t(MsgCode::MsgWin));
    CHECK_EQ(msgs[2].code, uint8_t(MsgCode::MsgDraw));
    Writer w;
    CHECK(EncodeMsgs(msgs, w));
    CHECK(w.vec() == raw);
}

TEST(Msg_truncated_fails) {
    std::vector<uint8_t> raw = BSelectCard();
    raw.resize(raw.size() - 3);  // cut into the second card entry
    Reader r(raw.data(), raw.size());
    std::vector<MsgEvent> msgs;
    std::string err;
    CHECK(!DecodeMsgs(r, msgs, err));
    CHECK(!err.empty());
}

TEST(Msg_unknown_code_fails) {
    std::vector<uint8_t> raw{200, 1, 2, 3};  // code 200 not in the table
    Reader r(raw.data(), raw.size());
    std::vector<MsgEvent> msgs;
    std::string err;
    CHECK(!DecodeMsgs(r, msgs, err));
}

TEST(Stoc_error_msg) {
    // Real-world case (from ygopromcp): deck rejected for a banned card.
    // code = (flag << 28) | card_code with flag = DECKERROR_LFLIST.
    std::vector<uint8_t> payload{0x02, 0x02, 0, 0, 0};
    uint32_t code = (DeckErrorLflist << 28) | 24224830u;
    P(payload, code);
    StocPacket pkt;
    std::string err;
    CHECK(DecodeStoc(payload.data(), payload.size(), pkt, err));
    CHECK(pkt.type == StocType::ErrorMsg);
    const auto& m = std::get<StocErrorMsg>(pkt.body);
    CHECK_EQ(m.msg, uint8_t(ErrDeckError));
    CHECK_EQ(m.code, code);
}

TEST(Stoc_game_msg_packet) {
    std::vector<uint8_t> raw = BDraw();
    std::vector<uint8_t> payload{0x01};
    payload.insert(payload.end(), raw.begin(), raw.end());
    StocPacket pkt;
    std::string err;
    CHECK(DecodeStoc(payload.data(), payload.size(), pkt, err));
    CHECK(pkt.type == StocType::GameMsg);
    const auto& msgs = std::get<std::vector<MsgEvent>>(pkt.body);
    CHECK_EQ(msgs.size(), size_t(1));
    CHECK_EQ(msgs[0].code, uint8_t(MsgCode::MsgDraw));
    // raw re-encode must be byte-identical
    std::vector<uint8_t> frame;
    CHECK(EncodeStoc(pkt, frame, err));
    CHECK_EQ(frame.size(), size_t(2 + payload.size()));
    CHECK_EQ(frame[2], uint8_t(0x01));
    // frame = [u16 LE len][payload]; expect must carry BOTH length bytes.
    std::vector<uint8_t> expect{LE16(static_cast<uint16_t>(payload.size())),
                                static_cast<uint8_t>(payload.size() >> 8)};
    expect.insert(expect.end(), payload.begin(), payload.end());
    CHECK(frame == expect);
}

TEST(Stoc_join_game) {
    HostInfo hi;
    hi.lflist = 5;
    hi.rule = 1;
    hi.start_lp = 8000;
    hi.start_hand = 5;
    hi.draw_count = 1;
    hi.time_limit = 180;
    Writer w;
    w.u8(0x12);
    EncodeHostInfo(w, hi);
    auto bytes = w.take();
    CHECK_EQ(bytes.size(), size_t(21));
    StocPacket pkt;
    std::string err;
    CHECK(DecodeStoc(bytes.data(), bytes.size(), pkt, err));
    const auto& m = std::get<StocJoinGame>(pkt.body);
    CHECK_EQ(m.info.lflist, uint32_t(5));
    CHECK_EQ(m.info.rule, uint8_t(1));
    CHECK_EQ(m.info.start_lp, int32_t(8000));
    CHECK_EQ(m.info.start_hand, uint8_t(5));
    CHECK_EQ(m.info.time_limit, uint16_t(180));
}

TEST(CardInfo_roundtrip) {
    CardInfo c;
    c.flag = QCode | QType | QAttack | QDefense | QLevel;
    c.code = 42;
    c.type = 0x21;
    c.attack = 2500;
    c.defense = 2000;
    c.level = 4;
    Writer w;
    EncodeCardInfo(w, c);
    auto bytes = w.take();
    Reader r(bytes.data(), bytes.size());
    CardInfo c2;
    CHECK(DecodeCardInfo(r, c2));
    CHECK_EQ(c2.flag, c.flag);
    CHECK_EQ(c2.code, c.code);
    CHECK_EQ(c2.type, c.type);
    CHECK_EQ(c2.attack, c.attack);
    CHECK_EQ(c2.level, c.level);
    CHECK(r.empty());
}

TEST(HostInfo_roundtrip) {
    HostInfo hi;
    hi.lflist = 3;
    hi.rule = 2;
    hi.mode = 1;
    hi.duel_rule = 4;
    hi.no_check_deck = 1;
    hi.no_shuffle_deck = 0;
    hi.start_lp = 4000;
    hi.start_hand = 4;
    hi.draw_count = 2;
    hi.time_limit = 300;
    Writer w;
    EncodeHostInfo(w, hi);
    auto bytes = w.take();
    CHECK_EQ(bytes.size(), size_t(20));
    Reader r(bytes.data(), bytes.size());
    HostInfo hi2;
    CHECK(DecodeHostInfo(r, hi2));
    CHECK_EQ(hi2.lflist, hi.lflist);
    CHECK_EQ(hi2.rule, hi.rule);
    CHECK_EQ(hi2.start_lp, hi.start_lp);
    CHECK_EQ(hi2.time_limit, hi.time_limit);
}

TEST(Utf16_conversion) {
    uint16_t buf[20] = {0};
    ToUtf16LE("yo", buf, 20);
    CHECK_EQ(buf[0], uint16_t('y'));
    CHECK_EQ(buf[1], uint16_t('o'));
    CHECK_EQ(buf[2], uint16_t(0));
    std::string back = FromUtf16LE(buf, 20);
    CHECK_EQ(back, std::string("yo"));
}
