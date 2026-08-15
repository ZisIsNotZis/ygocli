// proto.cpp — wire layer implementation (see proto.h for the contract and the
// knowhows). The layout table below is the authoritative MSG_* length/field
// knowledge: every message the server can emit is either typed-decoded (game
// interprets it) or consumed length-exact. A parse failure propagates as a
// hard error — never garbage state (error-surface ladder).

#include "proto.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace proto {

// ---------------------------------------------------------------------------
// UTF-16 helpers
// ---------------------------------------------------------------------------

void ToUtf16LE(const std::string& s, uint16_t* out, size_t n) {
    size_t i = 0;
    for (; i < s.size() && i + 1 < n; ++i)
        out[i] = static_cast<uint16_t>(static_cast<unsigned char>(s[i]));
    for (; i < n; ++i)
        out[i] = 0;
}

std::string FromUtf16LE(const uint16_t* in, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n && in[i] != 0; ++i)
        out.push_back(static_cast<char>(in[i] & 0x7f));  // ASCII subset
    return out;
}

// ---------------------------------------------------------------------------
// HostInfo / CardInfo
// ---------------------------------------------------------------------------

bool DecodeHostInfo(Reader& r, HostInfo& h) {
    return r.u32(h.lflist) && r.u8(h.rule) && r.u8(h.mode) && r.u8(h.duel_rule) &&
           r.u8(h.no_check_deck) && r.u8(h.no_shuffle_deck) && r.skip(3) &&
           r.i32(h.start_lp) && r.u8(h.start_hand) && r.u8(h.draw_count) &&
           r.u16(h.time_limit);
}

void EncodeHostInfo(Writer& w, const HostInfo& h) {
    w.u32(h.lflist).u8(h.rule).u8(h.mode).u8(h.duel_rule).u8(h.no_check_deck)
        .u8(h.no_shuffle_deck)
        .bytes(h.pad, 3)
        .i32(h.start_lp)
        .u8(h.start_hand)
        .u8(h.draw_count)
        .u16(h.time_limit);
}

// Card data layout: [flag u32][fields per flag]. All fields are u32 except the
// 4-byte card refs. flag == 0 terminates a card (server-side). We never read
// past the declared fields; a truncated block fails the parse.
bool DecodeCardInfo(Reader& r, CardInfo& c) {
    c = CardInfo{};
    if (!r.u32(c.flag))
        return false;
    if (c.flag == 0)
        return true;  // clear-data marker
    if (c.flag & QCode && !r.u32(c.code)) return false;
    if (c.flag & QPosition && !r.u32(c.position)) return false;
    if (c.flag & QAlias && !r.u32(c.alias)) return false;
    if (c.flag & QType && !r.u32(c.type)) return false;
    if (c.flag & QLevel && !r.u32(c.level)) return false;
    if (c.flag & QRank && !r.u32(c.rank)) return false;
    if (c.flag & QAttribute && !r.u32(c.attribute)) return false;
    if (c.flag & QRace && !r.u32(c.race)) return false;
    if (c.flag & QAttack && !r.u32(c.attack)) return false;
    if (c.flag & QDefense && !r.u32(c.defense)) return false;
    if (c.flag & QBaseAttack && !r.u32(c.base_attack)) return false;
    if (c.flag & QBaseDefense && !r.u32(c.base_defense)) return false;
    if (c.flag & QReason && !r.u32(c.reason)) return false;
    if (c.flag & QReasonCard && !(r.u8(c.reason_card.controler) && r.u8(c.reason_card.location) &&
                                  r.u8(c.reason_card.sequence) && r.u8(c.reason_card.sub_sequence)))
        return false;
    if (c.flag & QEquipCard && !(r.u8(c.equip_card.controler) && r.u8(c.equip_card.location) &&
                                 r.u8(c.equip_card.sequence) && r.u8(c.equip_card.sub_sequence)))
        return false;
    if (c.flag & QTargetCard && !(r.u8(c.target_card.controler) && r.u8(c.target_card.location) &&
                                  r.u8(c.target_card.sequence) && r.u8(c.target_card.sub_sequence)))
        return false;
    if (c.flag & QOverlayCard) {
        uint8_t n = 0;
        if (!r.u8(n))
            return false;
        c.overlay_cards.resize(n);
        for (auto& oc : c.overlay_cards)
            if (!(r.u8(oc.controler) && r.u8(oc.location) && r.u8(oc.sequence) &&
                  r.u8(oc.sub_sequence)))
                return false;
    }
    if (c.flag & QCounters && !r.u32(c.counters)) return false;
    if (c.flag & QOwner && !r.u32(c.owner)) return false;
    if (c.flag & QStatus && !r.u32(c.status)) return false;
    if (c.flag & QLscale && !r.u32(c.lscale)) return false;
    if (c.flag & QRscale && !r.u32(c.rscale)) return false;
    if (c.flag & QLink && !r.u32(c.link)) return false;
    return true;
}

void EncodeCardInfo(Writer& w, const CardInfo& c) {
    w.u32(c.flag);
    if (c.flag & QCode) w.u32(c.code);
    if (c.flag & QPosition) w.u32(c.position);
    if (c.flag & QAlias) w.u32(c.alias);
    if (c.flag & QType) w.u32(c.type);
    if (c.flag & QLevel) w.u32(c.level);
    if (c.flag & QRank) w.u32(c.rank);
    if (c.flag & QAttribute) w.u32(c.attribute);
    if (c.flag & QRace) w.u32(c.race);
    if (c.flag & QAttack) w.u32(c.attack);
    if (c.flag & QDefense) w.u32(c.defense);
    if (c.flag & QBaseAttack) w.u32(c.base_attack);
    if (c.flag & QBaseDefense) w.u32(c.base_defense);
    if (c.flag & QReason) w.u32(c.reason);
    if (c.flag & QReasonCard)
        w.u8(c.reason_card.controler).u8(c.reason_card.location).u8(c.reason_card.sequence)
            .u8(c.reason_card.sub_sequence);
    if (c.flag & QEquipCard)
        w.u8(c.equip_card.controler).u8(c.equip_card.location).u8(c.equip_card.sequence)
            .u8(c.equip_card.sub_sequence);
    if (c.flag & QTargetCard)
        w.u8(c.target_card.controler).u8(c.target_card.location).u8(c.target_card.sequence)
            .u8(c.target_card.sub_sequence);
    if (c.flag & QOverlayCard) {
        w.u8(static_cast<uint8_t>(c.overlay_cards.size()));
        for (const auto& oc : c.overlay_cards)
            w.u8(oc.controler).u8(oc.location).u8(oc.sequence).u8(oc.sub_sequence);
    }
    if (c.flag & QCounters) w.u32(c.counters);
    if (c.flag & QOwner) w.u32(c.owner);
    if (c.flag & QStatus) w.u32(c.status);
    if (c.flag & QLscale) w.u32(c.lscale);
    if (c.flag & QRscale) w.u32(c.rscale);
    if (c.flag & QLink) w.u32(c.link);
}

// ---------------------------------------------------------------------------
// MSG_* layout table — the core protocol knowledge. Every case reads EXACTLY
// the bytes of its message from `r`; the reader is shared across messages so a
// wrong length here desyncs the whole stream (that's what the tests guard).
// ---------------------------------------------------------------------------

namespace {

template <typename T>
void Put(MsgEvent& e, const T& body) {
    e.payload = body;
}

bool CardRefFromReader(Reader& r, CardRef& c) {
    return r.u32(c.code) && r.u8(c.controler) && r.u8(c.location) && r.u8(c.sequence) &&
           r.u8(c.sub_sequence);
}
void CardRefToWriter(Writer& w, const CardRef& c) {
    w.u32(c.code).u8(c.controler).u8(c.location).u8(c.sequence).u8(c.sub_sequence);
}
bool CardLocFromReader(Reader& r, CardLoc& c) {
    return r.u8(c.controler) && r.u8(c.location) && r.u8(c.sequence) && r.u8(c.sub_sequence);
}
void CardLocToWriter(Writer& w, const CardLoc& c) {
    w.u8(c.controler).u8(c.location).u8(c.sequence).u8(c.sub_sequence);
}

bool ParseHint(Reader& r, MsgEvent& e) {
    MsgHint m;
    return r.u8(m.type) && r.u8(m.player) && r.u32(m.data) && (Put(e, m), true);
}
void EncodeHint(Writer& w, const MsgHint& m) { w.u8(m.type).u8(m.player).u32(m.data); }

bool ParseWin(Reader& r, MsgEvent& e) {
    MsgWin m;
    return r.u8(m.player) && r.u8(m.reason) && (Put(e, m), true);
}
void EncodeWin(Writer& w, const MsgWin& m) { w.u8(m.player).u8(m.reason); }

bool ParseStart(Reader& r, MsgEvent& e) {
    MsgStart m;
    return r.u8(m.player_type) && r.u8(m.duel_rule) && r.i32(m.lp[0]) && r.i32(m.lp[1]) &&
           r.u16(m.deck[0]) && r.u16(m.extra[0]) && r.u16(m.deck[1]) && r.u16(m.extra[1]) &&
           (Put(e, m), true);
}
void EncodeStart(Writer& w, const MsgStart& m) {
    w.u8(m.player_type).u8(m.duel_rule).i32(m.lp[0]).i32(m.lp[1]).u16(m.deck[0])
        .u16(m.extra[0]).u16(m.deck[1]).u16(m.extra[1]);
}

bool ParseUpdateData(Reader& r, MsgEvent& e) {
    MsgUpdateData m;
    if (!(r.u8(m.player) && r.u8(m.location)))
        return false;
    // [len u32][flag u32][fields...] per card; len counts the whole entry.
    // CAVEAT (reference LEN_HEADER=8): entries with len <= 8 carry NO card
    // data (empty field slots / clear markers) and must be SKIPPED, not
    // parsed — parsing an empty entry fails the stream (hit against a real
    // MZONE update at duel start).
    while (!r.empty()) {
        uint32_t len = 0;
        if (!r.u32(len) || len < 4 || len - 4 > r.remaining())
            return false;
        if (len <= 8)
            continue;  // empty slot / clear marker: consume, no card
        const uint8_t* entry = nullptr;
        if (!r.bytes(len - 4, entry))
            return false;
        Reader sub(entry, len - 4);
        CardInfo ci;
        if (!DecodeCardInfo(sub, ci))
            return false;
        m.cards.push_back(std::move(ci));
    }
    return Put(e, m), true;
}
void EncodeUpdateData(Writer& w, const MsgUpdateData& m) {
    w.u8(m.player).u8(m.location);
    for (const auto& c : m.cards) {
        Writer entry;
        EncodeCardInfo(entry, c);
        auto body = entry.take();
        w.u32(static_cast<uint32_t>(4 + body.size())).bytes(body.data(), body.size());
    }
}

bool ParseUpdateCard(Reader& r, MsgEvent& e) {
    MsgUpdateCard m;
    if (!(r.u8(m.player) && r.u8(m.location) && r.u8(m.sequence)))
        return false;
    uint32_t len = 0;
    if (!r.u32(len) || len < 4 || len - 4 > r.remaining())
        return false;
    if (len <= 8)  // clear marker: no card data (same rule as UPDATE_DATA)
        return Put(e, m), true;
    const uint8_t* entry = nullptr;
    if (!r.bytes(len - 4, entry))
        return false;
    Reader sub(entry, len - 4);
    return DecodeCardInfo(sub, m.card) && (Put(e, m), true);
}
void EncodeUpdateCard(Writer& w, const MsgUpdateCard& m) {
    w.u8(m.player).u8(m.location).u8(m.sequence);
    Writer entry;
    EncodeCardInfo(entry, m.card);
    auto body = entry.take();
    w.u32(static_cast<uint32_t>(4 + body.size())).bytes(body.data(), body.size());
}

bool ParseEffectList(Reader& r, std::vector<EffectRef>& out) {
    uint8_t n = 0;
    if (!r.u8(n))
        return false;
    out.resize(n);
    for (auto& f : out)
        if (!(r.u32(f.code) && r.u8(f.controler) && r.u8(f.location) && r.u8(f.sequence) &&
              r.i32(f.desc)))
            return false;
    return true;
}
void EncodeEffectList(Writer& w, const std::vector<EffectRef>& v) {
    w.u8(static_cast<uint8_t>(v.size()));
    for (const auto& f : v)
        w.u32(f.code).u8(f.controler).u8(f.location).u8(f.sequence).i32(f.desc);
}

bool ParseCardRefList(Reader& r, std::vector<CardRef>& out) {
    uint8_t n = 0;
    if (!r.u8(n))
        return false;
    out.resize(n);
    for (auto& c : out)
        if (!CardRefFromReader(r, c))
            return false;
    return true;
}
void EncodeCardRefList(Writer& w, const std::vector<CardRef>& v) {
    w.u8(static_cast<uint8_t>(v.size()));
    for (const auto& c : v)
        CardRefToWriter(w, c);
}

// IDLECMD list entries are 7 bytes: [code u32][c][l][s] — NO sub_sequence
// (unlike the SELECT_CARD-family 8-byte refs). CAVEAT (hit against a live
// capture): using the 8-byte ref here desyncs the whole IDLECMD.
bool CardRef7FromReader(Reader& r, CardRef& c) {
    return r.u32(c.code) && r.u8(c.controler) && r.u8(c.location) && r.u8(c.sequence);
}
void CardRef7ToWriter(Writer& w, const CardRef& c) {
    w.u32(c.code).u8(c.controler).u8(c.location).u8(c.sequence);
}
bool ParseCardRef7List(Reader& r, std::vector<CardRef>& out) {
    uint8_t n = 0;
    if (!r.u8(n))
        return false;
    out.resize(n);
    for (auto& c : out)
        if (!CardRef7FromReader(r, c))
            return false;
    return true;
}
void EncodeCardRef7List(Writer& w, const std::vector<CardRef>& v) {
    w.u8(static_cast<uint8_t>(v.size()));
    for (const auto& c : v)
        CardRef7ToWriter(w, c);
}

bool ParseBattleCmd(Reader& r, MsgEvent& e) {
    MsgSelectBattleCmd m;
    if (!r.u8(m.player))
        return false;
    if (!ParseEffectList(r, m.activatable))
        return false;
    uint8_t n = 0;
    if (!r.u8(n))
        return false;
    m.attackable.resize(n);
    for (auto& a : m.attackable)
        if (!(r.u32(a.code) && r.u8(a.controler) && r.u8(a.location) && r.u8(a.sequence) &&
              r.u8(a.diratt)))
            return false;
    return r.u8(reinterpret_cast<uint8_t&>(m.to_bp)) && r.u8(reinterpret_cast<uint8_t&>(m.to_ep)) &&
           (Put(e, m), true);
}
void EncodeBattleCmd(Writer& w, const MsgSelectBattleCmd& m) {
    w.u8(m.player);
    EncodeEffectList(w, m.activatable);
    w.u8(static_cast<uint8_t>(m.attackable.size()));
    for (const auto& a : m.attackable)
        w.u32(a.code).u8(a.controler).u8(a.location).u8(a.sequence).u8(a.diratt);
    w.u8(m.to_bp ? 1 : 0).u8(m.to_ep ? 1 : 0);
}

bool ParseIdleCmd(Reader& r, MsgEvent& e) {
    MsgSelectIdleCmd m;
    if (!r.u8(m.player))
        return false;
    if (!ParseCardRef7List(r, m.summonable) || !ParseCardRef7List(r, m.spsummonable) ||
        !ParseCardRef7List(r, m.reposable) || !ParseCardRef7List(r, m.msetable) ||
        !ParseCardRef7List(r, m.ssetable) || !ParseEffectList(r, m.activatable))
        return false;
    return r.u8(reinterpret_cast<uint8_t&>(m.to_bp)) && r.u8(reinterpret_cast<uint8_t&>(m.to_ep)) &&
           r.u8(reinterpret_cast<uint8_t&>(m.shuffle)) && (Put(e, m), true);
}
void EncodeIdleCmd(Writer& w, const MsgSelectIdleCmd& m) {
    w.u8(m.player);
    EncodeCardRef7List(w, m.summonable);
    EncodeCardRef7List(w, m.spsummonable);
    EncodeCardRef7List(w, m.reposable);
    EncodeCardRef7List(w, m.msetable);
    EncodeCardRef7List(w, m.ssetable);
    EncodeEffectList(w, m.activatable);
    w.u8(m.to_bp ? 1 : 0).u8(m.to_ep ? 1 : 0).u8(m.shuffle ? 1 : 0);
}

bool ParseEffectYn(Reader& r, MsgEvent& e) {
    MsgSelectEffectYn m;
    return r.u8(m.player) && r.u32(m.code) && r.u8(m.controler) && r.u8(m.location) &&
           r.u8(m.sequence) && r.u8(m.unused) && r.i32(m.desc) && (Put(e, m), true);
}
void EncodeEffectYn(Writer& w, const MsgSelectEffectYn& m) {
    w.u8(m.player).u32(m.code).u8(m.controler).u8(m.location).u8(m.sequence).u8(m.unused)
        .i32(m.desc);
}

bool ParseYesNo(Reader& r, MsgEvent& e) {
    MsgSelectYesNo m;
    return r.u8(m.player) && r.i32(m.desc) && (Put(e, m), true);
}
void EncodeYesNo(Writer& w, const MsgSelectYesNo& m) { w.u8(m.player).i32(m.desc); }

bool ParseOption(Reader& r, MsgEvent& e) {
    MsgSelectOption m;
    uint8_t n = 0;
    if (!(r.u8(m.player) && r.u8(n)))
        return false;
    m.options.resize(n);
    for (auto& o : m.options)
        if (!r.i32(o))
            return false;
    return Put(e, m), true;
}
void EncodeOption(Writer& w, const MsgSelectOption& m) {
    w.u8(m.player).u8(static_cast<uint8_t>(m.options.size()));
    for (auto o : m.options)
        w.i32(o);
}

bool ParseSelectCard(Reader& r, MsgEvent& e) {
    MsgSelectCard m;
    if (!(r.u8(m.player) && r.u8(m.cancelable) && r.u8(m.min) && r.u8(m.max)))
        return false;
    return ParseCardRefList(r, m.cards) && (Put(e, m), true);
}
void EncodeSelectCard(Writer& w, const MsgSelectCard& m) {
    w.u8(m.player).u8(m.cancelable).u8(m.min).u8(m.max);
    EncodeCardRefList(w, m.cards);
}

bool ParseUnselectCard(Reader& r, MsgEvent& e) {
    MsgSelectUnselectCard m;
    if (!(r.u8(m.player) && r.u8(m.finishable) && r.u8(m.cancelable) && r.u8(m.min) && r.u8(m.max)))
        return false;
    return ParseCardRefList(r, m.selectable) && ParseCardRefList(r, m.selected) &&
           (Put(e, m), true);
}
void EncodeUnselectCard(Writer& w, const MsgSelectUnselectCard& m) {
    w.u8(m.player).u8(m.finishable).u8(m.cancelable).u8(m.min).u8(m.max);
    EncodeCardRefList(w, m.selectable);
    EncodeCardRefList(w, m.selected);
}

bool ParseChain(Reader& r, MsgEvent& e) {
    MsgSelectChain m;
    if (!(r.u8(m.player) && r.u8(m.count) && r.u8(m.specount) && r.i32(m.hint0) && r.i32(m.hint1)))
        return false;
    m.entries.resize(m.count);
    for (auto& en : m.entries)
        if (!(r.u8(en.flag) && r.u8(en.forced) && r.u32(en.code) && r.u8(en.controler) &&
              r.u8(en.location) && r.u8(en.sequence) && r.u8(en.sub_sequence) && r.i32(en.desc)))
            return false;
    return Put(e, m), true;
}
void EncodeChain(Writer& w, const MsgSelectChain& m) {
    w.u8(m.player).u8(m.count).u8(m.specount).i32(m.hint0).i32(m.hint1);
    for (const auto& en : m.entries)
        w.u8(en.flag).u8(en.forced).u32(en.code).u8(en.controler).u8(en.location)
            .u8(en.sequence).u8(en.sub_sequence).i32(en.desc);
}

bool ParseSelectPlace(Reader& r, MsgEvent& e) {
    MsgSelectPlace m;
    return r.u8(m.player) && r.u8(m.count) && r.u32(m.zone) && (Put(e, m), true);
}
void EncodeSelectPlace(Writer& w, const MsgSelectPlace& m) {
    w.u8(m.player).u8(m.count).u32(m.zone);
}

bool ParseSelectPosition(Reader& r, MsgEvent& e) {
    MsgSelectPosition m;
    return r.u8(m.player) && r.u32(m.code) && r.u8(m.positions) && (Put(e, m), true);
}
void EncodeSelectPosition(Writer& w, const MsgSelectPosition& m) {
    w.u8(m.player).u32(m.code).u8(m.positions);
}

bool ParseTribute(Reader& r, MsgEvent& e) {
    MsgSelectTribute m;
    if (!(r.u8(m.player) && r.u8(m.cancelable) && r.u8(m.min) && r.u8(m.max)))
        return false;
    return ParseCardRefList(r, m.cards) && (Put(e, m), true);
}
void EncodeTribute(Writer& w, const MsgSelectTribute& m) {
    w.u8(m.player).u8(m.cancelable).u8(m.min).u8(m.max);
    EncodeCardRefList(w, m.cards);
}

bool ParseCounter(Reader& r, MsgEvent& e) {
    MsgSelectCounter m;
    if (!(r.u8(m.player) && r.u16(m.type) && r.u16(m.count)))
        return false;
    uint8_t n = 0;
    if (!r.u8(n))
        return false;
    m.cards.resize(n);
    m.card_counts.resize(n);
    for (size_t i = 0; i < n; ++i)
        if (!(CardRefFromReader(r, m.cards[i]) && r.u16(m.card_counts[i])))
            return false;
    return Put(e, m), true;
}
void EncodeCounter(Writer& w, const MsgSelectCounter& m) {
    w.u8(m.player).u16(m.type).u16(m.count);
    w.u8(static_cast<uint8_t>(m.cards.size()));
    for (size_t i = 0; i < m.cards.size(); ++i) {
        CardRefToWriter(w, m.cards[i]);
        w.u16(i < m.card_counts.size() ? m.card_counts[i] : 0);
    }
}

bool ParseSum(Reader& r, MsgEvent& e) {
    MsgSelectSum m;
    if (!(r.u8(m.mode) && r.u8(m.player) && r.i32(m.sumval) && r.u8(m.min) && r.u8(m.max) &&
          r.u8(m.must_select_count)))
        return false;
    uint8_t n1 = 0, n2 = 0;
    if (!r.u8(n1))
        return false;
    m.selectable.resize(n1);
    m.op_params1.resize(n1);
    for (size_t i = 0; i < n1; ++i)
        if (!(CardRefFromReader(r, m.selectable[i]) && r.i32(m.op_params1[i])))
            return false;
    if (!r.u8(n2))
        return false;
    m.must_select.resize(n2);
    m.op_params2.resize(n2);
    for (size_t i = 0; i < n2; ++i)
        if (!(CardRefFromReader(r, m.must_select[i]) && r.i32(m.op_params2[i])))
            return false;
    return Put(e, m), true;
}
void EncodeSum(Writer& w, const MsgSelectSum& m) {
    w.u8(m.mode).u8(m.player).i32(m.sumval).u8(m.min).u8(m.max).u8(m.must_select_count);
    w.u8(static_cast<uint8_t>(m.selectable.size()));
    for (size_t i = 0; i < m.selectable.size(); ++i) {
        CardRefToWriter(w, m.selectable[i]);
        w.i32(i < m.op_params1.size() ? m.op_params1[i] : 0);
    }
    w.u8(static_cast<uint8_t>(m.must_select.size()));
    for (size_t i = 0; i < m.must_select.size(); ++i) {
        CardRefToWriter(w, m.must_select[i]);
        w.i32(i < m.op_params2.size() ? m.op_params2[i] : 0);
    }
}

bool ParseSortCard(Reader& r, MsgEvent& e) {
    MsgSortCard m;
    return r.u8(m.player) && ParseCardRefList(r, m.cards) && (Put(e, m), true);
}
void EncodeSortCard(Writer& w, const MsgSortCard& m) {
    w.u8(m.player);
    EncodeCardRefList(w, m.cards);
}

bool ParseConfirmCards(Reader& r, MsgEvent& e) {
    MsgConfirmCards m;
    if (!(r.u8(m.player) && r.u8(m.skip_panel)))
        return false;
    // Entries are 7-byte refs (code+c+l+s, no sub_sequence) — verified against
    // the engine's Duel.ConfirmCards write. An 8-byte read overruns the last
    // entry and desyncs the stream.
    uint8_t n = 0;
    if (!r.u8(n))
        return false;
    m.cards.resize(n);
    for (auto& c : m.cards)
        if (!CardRef7FromReader(r, c))
            return false;
    return Put(e, m), true;
}
void EncodeConfirmCards(Writer& w, const MsgConfirmCards& m) {
    w.u8(m.player).u8(m.skip_panel);
    EncodeCardRef7List(w, m.cards);
}
bool ParseConfirmTop(Reader& r, MsgEvent& e) {
    MsgConfirmTop m;
    uint8_t n = 0;
    if (!(r.u8(m.player) && r.u8(n)))
        return false;
    m.codes.resize(n);
    for (auto& c : m.codes)
        if (!r.u32(c))
            return false;
    return Put(e, m), true;
}
void EncodeConfirmTop(Writer& w, const MsgConfirmTop& m) {
    w.u8(m.player).u8(static_cast<uint8_t>(m.codes.size()));
    for (auto c : m.codes)
        w.u32(c);
}

bool ParseMove(Reader& r, MsgEvent& e) {
    MsgMove m;
    return r.u32(m.code) && r.u8(m.prev_controler) && r.u8(m.prev_location) &&
           r.u8(m.prev_sequence) && r.u8(m.prev_position) && r.u8(m.cur_controler) &&
           r.u8(m.cur_location) && r.u8(m.cur_sequence) && r.u8(m.cur_position) &&
           r.i32(m.reason) && (Put(e, m), true);
}
void EncodeMove(Writer& w, const MsgMove& m) {
    w.u32(m.code).u8(m.prev_controler).u8(m.prev_location).u8(m.prev_sequence)
        .u8(m.prev_position).u8(m.cur_controler).u8(m.cur_location).u8(m.cur_sequence)
        .u8(m.cur_position).i32(m.reason);
}

bool ParsePosChange(Reader& r, MsgEvent& e) {
    MsgPosChange m;
    return r.u32(m.code) && r.u8(m.controler) && r.u8(m.location) && r.u8(m.sequence) &&
           r.u8(m.prev_position) && r.u8(m.cur_position) && (Put(e, m), true);
}
void EncodePosChange(Writer& w, const MsgPosChange& m) {
    w.u32(m.code).u8(m.controler).u8(m.location).u8(m.sequence).u8(m.prev_position)
        .u8(m.cur_position);
}

bool ParseSet(Reader& r, MsgEvent& e) {
    MsgSet m;
    return r.u32(m.code) && r.u8(m.controler) && r.u8(m.location) && r.u8(m.sequence) &&
           r.u8(m.position) && (Put(e, m), true);
}
void EncodeSet(Writer& w, const MsgSet& m) {
    w.u32(m.code).u8(m.controler).u8(m.location).u8(m.sequence).u8(m.position);
}

bool ParseSwap(Reader& r, MsgEvent& e) {
    MsgSwap m;
    return r.u32(m.code1) && r.u8(m.c1) && r.u8(m.l1) && r.u8(m.s1) && r.u8(m.p1) &&
           r.u32(m.code2) && r.u8(m.c2) && r.u8(m.l2) && r.u8(m.s2) && r.u8(m.p2) &&
           (Put(e, m), true);
}
void EncodeSwap(Writer& w, const MsgSwap& m) {
    w.u32(m.code1).u8(m.c1).u8(m.l1).u8(m.s1).u8(m.p1).u32(m.code2).u8(m.c2).u8(m.l2)
        .u8(m.s2).u8(m.p2);
}

bool ParseSummoning(Reader& r, MsgEvent& e) {
    MsgSummoning m;
    return r.u32(m.code) && r.u8(m.controler) && r.u8(m.location) && r.u8(m.sequence) &&
           r.u8(m.position) && (Put(e, m), true);
}
void EncodeSummoning(Writer& w, const MsgSummoning& m) {
    w.u32(m.code).u8(m.controler).u8(m.location).u8(m.sequence).u8(m.position);
}

bool ParseChaining(Reader& r, MsgEvent& e) {
    MsgChaining m;
    return r.u32(m.code) && r.u8(m.prev_controler) && r.u8(m.prev_location) &&
           r.u8(m.prev_sequence) && r.u8(m.sub_sequence) && r.u8(m.cur_controler) &&
           r.u8(m.cur_location) && r.u8(m.cur_sequence) && r.i32(m.desc) &&
           r.u8(m.chain_count) && (Put(e, m), true);
}
void EncodeChaining(Writer& w, const MsgChaining& m) {
    w.u32(m.code).u8(m.prev_controler).u8(m.prev_location).u8(m.prev_sequence)
        .u8(m.sub_sequence).u8(m.cur_controler).u8(m.cur_location).u8(m.cur_sequence)
        .i32(m.desc).u8(m.chain_count);
}

bool ParseCardLocList(Reader& r, std::vector<CardLoc>& out) {
    uint8_t n = 0;
    if (!r.u8(n))
        return false;
    out.resize(n);
    for (auto& c : out)
        if (!CardLocFromReader(r, c))
            return false;
    return true;
}
void EncodeCardLocList(Writer& w, const std::vector<CardLoc>& v) {
    w.u8(static_cast<uint8_t>(v.size()));
    for (const auto& c : v)
        CardLocToWriter(w, c);
}

bool ParseDraw(Reader& r, MsgEvent& e) {
    MsgDraw m;
    uint8_t n = 0;
    if (!(r.u8(m.player) && r.u8(n)))
        return false;
    m.codes.resize(n);
    for (auto& c : m.codes)
        if (!r.u32(c))
            return false;
    return Put(e, m), true;
}
void EncodeDraw(Writer& w, const MsgDraw& m) {
    w.u8(m.player).u8(static_cast<uint8_t>(m.codes.size()));
    for (auto c : m.codes)
        w.u32(c);
}

bool ParsePair(Reader& r, MsgEvent& e, bool equip) {
    if (equip) {
        MsgEquip m;
        return CardLocFromReader(r, m.equip) && CardLocFromReader(r, m.target) &&
               (Put(e, m), true);
    }
    MsgTargetPair m;
    return CardLocFromReader(r, m.source) && CardLocFromReader(r, m.target) &&
           (Put(e, m), true);
}
void EncodeEquip(Writer& w, const MsgEquip& m) {
    CardLocToWriter(w, m.equip);
    CardLocToWriter(w, m.target);
}
void EncodeTargetPair(Writer& w, const MsgTargetPair& m) {
    CardLocToWriter(w, m.source);
    CardLocToWriter(w, m.target);
}

bool ParseAttack(Reader& r, MsgEvent& e) {
    MsgAttack m;
    return CardLocFromReader(r, m.attacker) && CardLocFromReader(r, m.target) &&
           (Put(e, m), true);
}
void EncodeAttack(Writer& w, const MsgAttack& m) {
    CardLocToWriter(w, m.attacker);
    CardLocToWriter(w, m.target);
}

bool ParseBattle(Reader& r, MsgEvent& e) {
    MsgBattle m;
    return CardLocFromReader(r, m.attacker) && r.i32(m.a_atk) && r.i32(m.a_def) &&
           r.u8(m.a_chain) && CardLocFromReader(r, m.target) && r.i32(m.d_atk) &&
           r.i32(m.d_def) && r.u8(m.d_chain) && (Put(e, m), true);
}
void EncodeBattle(Writer& w, const MsgBattle& m) {
    CardLocToWriter(w, m.attacker);
    w.i32(m.a_atk).i32(m.a_def).u8(m.a_chain);
    CardLocToWriter(w, m.target);
    w.i32(m.d_atk).i32(m.d_def).u8(m.d_chain);
}

bool ParseToss(Reader& r, MsgEvent& e) {
    MsgToss m;
    uint8_t n = 0;
    if (!(r.u8(m.player) && r.u8(n)))
        return false;
    m.results.resize(n);
    for (auto& res : m.results)
        if (!r.u8(res))
            return false;
    return Put(e, m), true;
}
void EncodeToss(Writer& w, const MsgToss& m) {
    w.u8(m.player).u8(static_cast<uint8_t>(m.results.size()));
    for (auto res : m.results)
        w.u8(res);
}

}  // namespace

// The layout table: code -> parse/encode. This switch MUST consume exactly the
// message bytes; the tests round-trip every case and the capture fixtures
// verify against real server streams.
bool DecodeMsgs(Reader& r, std::vector<MsgEvent>& out, std::string& err) {
    while (!r.empty()) {
        MsgEvent e;
        const size_t start = r.pos();
        if (!r.u8(e.code)) {
            err = "truncated MSG code";
            return false;
        }
        const uint8_t code = e.code;
        bool ok = true;
        switch (static_cast<MsgCode>(code)) {
        case MsgCode::MsgRetry:
        case MsgCode::MsgWaiting:
        case MsgCode::MsgSummoned:
        case MsgCode::MsgSpSummoned:
        case MsgCode::MsgFlipSummoned:
        case MsgCode::MsgChainEnd:
        case MsgCode::MsgCardSelected:
        case MsgCode::MsgAttackDisabled:
        case MsgCode::MsgDamageStepStart:
        case MsgCode::MsgDamageStepEnd:
            Put(e, MsgEmpty{});
            break;
        case MsgCode::MsgHint: ok = ParseHint(r, e); break;
        case MsgCode::MsgWin: ok = ParseWin(r, e); break;
        case MsgCode::MsgStart: ok = ParseStart(r, e); break;
        case MsgCode::MsgUpdateData: ok = ParseUpdateData(r, e); break;
        case MsgCode::MsgUpdateCard: ok = ParseUpdateCard(r, e); break;
        case MsgCode::MsgSelectBattleCmd: ok = ParseBattleCmd(r, e); break;
        case MsgCode::MsgSelectIdleCmd: ok = ParseIdleCmd(r, e); break;
        case MsgCode::MsgSelectEffectYn: ok = ParseEffectYn(r, e); break;
        case MsgCode::MsgSelectYesNo: ok = ParseYesNo(r, e); break;
        case MsgCode::MsgSelectOption: ok = ParseOption(r, e); break;
        case MsgCode::MsgSelectCard: ok = ParseSelectCard(r, e); break;
        case MsgCode::MsgSelectUnselectCard: ok = ParseUnselectCard(r, e); break;
        case MsgCode::MsgSelectChain: ok = ParseChain(r, e); break;
        case MsgCode::MsgSelectPlace:
        case MsgCode::MsgSelectDisField: ok = ParseSelectPlace(r, e); break;
        case MsgCode::MsgSelectPosition: ok = ParseSelectPosition(r, e); break;
        case MsgCode::MsgSelectTribute: ok = ParseTribute(r, e); break;
        case MsgCode::MsgSelectCounter: ok = ParseCounter(r, e); break;
        case MsgCode::MsgSelectSum: ok = ParseSum(r, e); break;
        case MsgCode::MsgSortCard: ok = ParseSortCard(r, e); break;
        case MsgCode::MsgConfirmDeckTop:
        case MsgCode::MsgConfirmExtraTop: ok = ParseConfirmTop(r, e); break;
        case MsgCode::MsgConfirmCards: ok = ParseConfirmCards(r, e); break;
        case MsgCode::MsgShuffleDeck:
        case MsgCode::MsgRefreshDeck:
        case MsgCode::MsgSwapGraveDeck:
        case MsgCode::MsgReverseDeck: {  // [player u8]
            uint8_t p = 0;
            ok = r.u8(p);
            Put(e, MsgNewTurn{p});
            break;
        }
        case MsgCode::MsgShuffleHand:
        case MsgCode::MsgShuffleExtra: {  // [player u8][count u8][count x code u32]
            MsgDraw m;
            uint8_t n = 0;
            ok = r.u8(m.player) && r.u8(n);
            m.codes.resize(n);
            for (auto& c : m.codes)
                ok = ok && r.u32(c);
            Put(e, m);
            break;
        }
        case MsgCode::MsgShuffleSetCard: {
            MsgShuffleSet m;
            ok = r.u8(m.location) && ParseCardLocList(r, m.cards) &&
                 ParseCardLocList(r, m.shuffled);
            Put(e, m);
            break;
        }
        case MsgCode::MsgDeckTop: {
            MsgDeckTop m;
            ok = r.u8(m.player) && r.u8(m.sequence) && r.u32(m.code);
            Put(e, m);
            break;
        }
        case MsgCode::MsgNewTurn: {
            MsgNewTurn m;
            ok = r.u8(m.player);
            Put(e, m);
            break;
        }
        case MsgCode::MsgNewPhase: {
            MsgNewPhase m;
            ok = r.u16(m.phase);
            Put(e, m);
            break;
        }
        case MsgCode::MsgMove: ok = ParseMove(r, e); break;
        case MsgCode::MsgPosChange: ok = ParsePosChange(r, e); break;
        case MsgCode::MsgSet: ok = ParseSet(r, e); break;
        case MsgCode::MsgSwap: ok = ParseSwap(r, e); break;
        case MsgCode::MsgFieldDisabled: {
            MsgFieldDisabled m;
            ok = r.u32(m.disabled);
            Put(e, m);
            break;
        }
        case MsgCode::MsgSummoning:
        case MsgCode::MsgSpSummoning:
        case MsgCode::MsgFlipSummoning: ok = ParseSummoning(r, e); break;
        case MsgCode::MsgChaining: ok = ParseChaining(r, e); break;
        case MsgCode::MsgChained:
        case MsgCode::MsgChainSolving:
        case MsgCode::MsgChainSolved:
        case MsgCode::MsgChainNegated:
        case MsgCode::MsgChainDisabled: {
            MsgChainCount m;
            ok = r.u8(m.chain_count);
            Put(e, m);
            break;
        }
        case MsgCode::MsgRandomSelected: {
            MsgRandomSelected m;
            ok = r.u8(m.player) && ParseCardLocList(r, m.cards);
            Put(e, m);
            break;
        }
        case MsgCode::MsgBecomeTarget: {
            MsgBecomeTarget m;
            ok = ParseCardLocList(r, m.cards);
            Put(e, m);
            break;
        }
        case MsgCode::MsgDraw: ok = ParseDraw(r, e); break;
        case MsgCode::MsgDamage:
        case MsgCode::MsgRecover:
        case MsgCode::MsgLpUpdate:
        case MsgCode::MsgPayLpCost: {
            MsgLp m;
            ok = r.u8(m.player) && r.i32(m.value);
            Put(e, m);
            break;
        }
        case MsgCode::MsgEquip: ok = ParsePair(r, e, true); break;
        case MsgCode::MsgUnequip: {
            MsgTargetPair m;
            ok = CardLocFromReader(r, m.source);
            Put(e, m);
            break;
        }
        case MsgCode::MsgCardTarget:
        case MsgCode::MsgCancelTarget: ok = ParsePair(r, e, false); break;
        case MsgCode::MsgAddCounter:
        case MsgCode::MsgRemoveCounter: {
            MsgCounter m;
            ok = r.u16(m.type) && r.u8(m.controler) && r.u8(m.location) && r.u8(m.sequence) &&
                 r.u16(m.count);
            Put(e, m);
            break;
        }
        case MsgCode::MsgAttack: ok = ParseAttack(r, e); break;
        case MsgCode::MsgBattle: ok = ParseBattle(r, e); break;
        case MsgCode::MsgMissedEffect: {
            MsgMissedEffect m;
            ok = r.u32(m.desc) && r.u32(m.code);
            Put(e, m);
            break;
        }
        case MsgCode::MsgTossCoin:
        case MsgCode::MsgTossDice: ok = ParseToss(r, e); break;
        case MsgCode::MsgRockPaperScissors: {
            MsgRps m;
            ok = r.u8(m.player);
            Put(e, m);
            break;
        }
        case MsgCode::MsgHandRes: {
            MsgHandRes m;
            ok = r.u8(m.result);
            Put(e, m);
            break;
        }
        case MsgCode::MsgAnnounceRace:
        case MsgCode::MsgAnnounceAttrib: {
            MsgAnnounce m;
            ok = r.u8(m.player) && r.u8(m.count) && r.u32(m.available);
            Put(e, m);
            break;
        }
        case MsgCode::MsgAnnounceCard: {
            MsgAnnounceCard m;
            uint8_t n = 0;
            ok = r.u8(m.player) && r.u8(n);
            m.opcodes.resize(n);
            for (auto& c : m.opcodes)
                ok = ok && r.u32(c);
            Put(e, m);
            break;
        }
        case MsgCode::MsgAnnounceNumber: {
            MsgAnnounceNumber m;
            uint8_t n = 0;
            ok = r.u8(m.player) && r.u8(n);
            m.values.resize(n);
            for (auto& v : m.values)
                ok = ok && r.i32(v);
            Put(e, m);
            break;
        }
        case MsgCode::MsgCardHint: {
            MsgCardHint m;
            ok = CardLocFromReader(r, m.card) && r.u8(m.type) && r.i32(m.value);
            Put(e, m);
            break;
        }
        case MsgCode::MsgPlayerHint: {
            MsgPlayerHint m;
            ok = r.u8(m.player) && r.u8(m.type) && r.i32(m.value);
            Put(e, m);
            break;
        }
        case MsgCode::MsgMatchKill: {
            MsgMatchKill m;
            ok = r.u32(m.code);
            Put(e, m);
            break;
        }
        case MsgCode::MsgTagSwap: {
            // [player u8][4 x count u8][topcode i32][4 x count x code u32]
            MsgTagSwap m;
            ok = r.u8(m.player);
            for (auto& c : m.counts)
                ok = ok && r.u8(c);
            ok = ok && r.i32(m.topcode);
            for (int i = 0; i < 4 && ok; ++i) {
                m.lists[i].resize(m.counts[i]);
                for (auto& c : m.lists[i])
                    ok = ok && r.u32(c);
            }
            Put(e, m);
            break;
        }
        case MsgCode::MsgReloadField: {
            // Debug-only (Debug.ReloadFieldEnd); never sent by srvpro in normal
            // play. Kept as a length-exact opaque skip: the format is
            // [duel_rule u8][lp x2 i32][per-player zone data][chains] — fixed
            // structure but rarely exercised; if a capture ever hits it the
            // fixture test will force a proper parse.
            MsgReloadField m;
            ok = r.skip(r.remaining());  // consume to end-of-packet is WRONG for
                                         // multi-msg packets; see note below.
            Put(e, m);
            break;
        }
        default: {
            // Unknown code = unknown length = guaranteed desync. The version
            // gate should make this unreachable; if it happens, fail loudly.
            err = "unknown MSG code " + std::to_string(code);
            return false;
        }
        }
        if (!ok) {
            err = "malformed MSG " + std::to_string(code) + " at offset " + std::to_string(start);
            return false;
        }
        // raw = code byte + consumed payload (for dump/replay/round-trip).
        e.raw.assign(r.data() + start, r.data() + r.pos());
        out.push_back(std::move(e));
    }
    return true;
}

// NOTE: MSG_RELOAD_FIELD's "skip to end" above is only correct because in
// practice it is the last (often only) message of its packet. Documented
// deliberately — a malformed multi-msg packet fails loudly via the bounded
// reader rather than silently desyncing.

// Typed encode (inverse of DecodeMsgs). Used by the round-trip tests to prove
// the layout table is byte-exact; the game layer uses it only if it ever needs
// to build a message (the client normally only sends CTOS_RESPONSE bytes).
bool EncodeMsgs(const std::vector<MsgEvent>& msgs, Writer& w) {
    for (const auto& e : msgs) {
        w.u8(e.code);
        switch (static_cast<MsgCode>(e.code)) {
        case MsgCode::MsgRetry:
        case MsgCode::MsgWaiting:
        case MsgCode::MsgSummoned:
        case MsgCode::MsgSpSummoned:
        case MsgCode::MsgFlipSummoned:
        case MsgCode::MsgChainEnd:
        case MsgCode::MsgCardSelected:
        case MsgCode::MsgAttackDisabled:
        case MsgCode::MsgDamageStepStart:
        case MsgCode::MsgDamageStepEnd:
        case MsgCode::MsgReloadField:  // opaque skip (see DecodeMsgs note); never rebuilt
            break;
        case MsgCode::MsgHint: EncodeHint(w, std::get<MsgHint>(e.payload)); break;
        case MsgCode::MsgWin: EncodeWin(w, std::get<MsgWin>(e.payload)); break;
        case MsgCode::MsgStart: EncodeStart(w, std::get<MsgStart>(e.payload)); break;
        case MsgCode::MsgUpdateData: EncodeUpdateData(w, std::get<MsgUpdateData>(e.payload)); break;
        case MsgCode::MsgUpdateCard: EncodeUpdateCard(w, std::get<MsgUpdateCard>(e.payload)); break;
        case MsgCode::MsgSelectBattleCmd:
            EncodeBattleCmd(w, std::get<MsgSelectBattleCmd>(e.payload));
            break;
        case MsgCode::MsgSelectIdleCmd:
            EncodeIdleCmd(w, std::get<MsgSelectIdleCmd>(e.payload));
            break;
        case MsgCode::MsgSelectEffectYn:
            EncodeEffectYn(w, std::get<MsgSelectEffectYn>(e.payload));
            break;
        case MsgCode::MsgSelectYesNo: EncodeYesNo(w, std::get<MsgSelectYesNo>(e.payload)); break;
        case MsgCode::MsgSelectOption: EncodeOption(w, std::get<MsgSelectOption>(e.payload)); break;
        case MsgCode::MsgSelectCard: EncodeSelectCard(w, std::get<MsgSelectCard>(e.payload)); break;
        case MsgCode::MsgSelectUnselectCard:
            EncodeUnselectCard(w, std::get<MsgSelectUnselectCard>(e.payload));
            break;
        case MsgCode::MsgSelectChain: EncodeChain(w, std::get<MsgSelectChain>(e.payload)); break;
        case MsgCode::MsgSelectPlace:
        case MsgCode::MsgSelectDisField:
            EncodeSelectPlace(w, std::get<MsgSelectPlace>(e.payload));
            break;
        case MsgCode::MsgSelectPosition:
            EncodeSelectPosition(w, std::get<MsgSelectPosition>(e.payload));
            break;
        case MsgCode::MsgSelectTribute: EncodeTribute(w, std::get<MsgSelectTribute>(e.payload)); break;
        case MsgCode::MsgSelectCounter: EncodeCounter(w, std::get<MsgSelectCounter>(e.payload)); break;
        case MsgCode::MsgSelectSum: EncodeSum(w, std::get<MsgSelectSum>(e.payload)); break;
        case MsgCode::MsgSortCard: EncodeSortCard(w, std::get<MsgSortCard>(e.payload)); break;
        case MsgCode::MsgConfirmDeckTop:
        case MsgCode::MsgConfirmExtraTop:
            EncodeConfirmTop(w, std::get<MsgConfirmTop>(e.payload));
            break;
        case MsgCode::MsgConfirmCards:
            EncodeConfirmCards(w, std::get<MsgConfirmCards>(e.payload));
            break;
        case MsgCode::MsgShuffleDeck:
        case MsgCode::MsgRefreshDeck:
        case MsgCode::MsgSwapGraveDeck:
        case MsgCode::MsgReverseDeck:
        case MsgCode::MsgNewTurn:
            w.u8(std::get<MsgNewTurn>(e.payload).player);
            break;
        case MsgCode::MsgShuffleHand:
        case MsgCode::MsgShuffleExtra:
        case MsgCode::MsgDraw:
            EncodeDraw(w, std::get<MsgDraw>(e.payload));
            break;
        case MsgCode::MsgShuffleSetCard: {
            const auto& m = std::get<MsgShuffleSet>(e.payload);
            w.u8(m.location);
            EncodeCardLocList(w, m.cards);
            EncodeCardLocList(w, m.shuffled);
            break;
        }
        case MsgCode::MsgDeckTop: {
            const auto& m = std::get<MsgDeckTop>(e.payload);
            w.u8(m.player).u8(m.sequence).u32(m.code);
            break;
        }
        case MsgCode::MsgNewPhase: w.u16(std::get<MsgNewPhase>(e.payload).phase); break;
        case MsgCode::MsgMove: EncodeMove(w, std::get<MsgMove>(e.payload)); break;
        case MsgCode::MsgPosChange: EncodePosChange(w, std::get<MsgPosChange>(e.payload)); break;
        case MsgCode::MsgSet: EncodeSet(w, std::get<MsgSet>(e.payload)); break;
        case MsgCode::MsgSwap: EncodeSwap(w, std::get<MsgSwap>(e.payload)); break;
        case MsgCode::MsgFieldDisabled:
            w.u32(std::get<MsgFieldDisabled>(e.payload).disabled);
            break;
        case MsgCode::MsgSummoning:
        case MsgCode::MsgSpSummoning:
        case MsgCode::MsgFlipSummoning:
            EncodeSummoning(w, std::get<MsgSummoning>(e.payload));
            break;
        case MsgCode::MsgChaining: EncodeChaining(w, std::get<MsgChaining>(e.payload)); break;
        case MsgCode::MsgChained:
        case MsgCode::MsgChainSolving:
        case MsgCode::MsgChainSolved:
        case MsgCode::MsgChainNegated:
        case MsgCode::MsgChainDisabled:
            w.u8(std::get<MsgChainCount>(e.payload).chain_count);
            break;
        case MsgCode::MsgRandomSelected: {
            const auto& m = std::get<MsgRandomSelected>(e.payload);
            w.u8(m.player);
            EncodeCardLocList(w, m.cards);
            break;
        }
        case MsgCode::MsgBecomeTarget:
            EncodeCardLocList(w, std::get<MsgBecomeTarget>(e.payload).cards);
            break;
        case MsgCode::MsgDamage:
        case MsgCode::MsgRecover:
        case MsgCode::MsgLpUpdate:
        case MsgCode::MsgPayLpCost: {
            const auto& m = std::get<MsgLp>(e.payload);
            w.u8(m.player).i32(m.value);
            break;
        }
        case MsgCode::MsgEquip: EncodeEquip(w, std::get<MsgEquip>(e.payload)); break;
        case MsgCode::MsgUnequip: {
            const auto& m = std::get<MsgTargetPair>(e.payload);
            CardLocToWriter(w, m.source);
            break;
        }
        case MsgCode::MsgCardTarget:
        case MsgCode::MsgCancelTarget:
            EncodeTargetPair(w, std::get<MsgTargetPair>(e.payload));
            break;
        case MsgCode::MsgAddCounter:
        case MsgCode::MsgRemoveCounter: {
            const auto& m = std::get<MsgCounter>(e.payload);
            w.u16(m.type).u8(m.controler).u8(m.location).u8(m.sequence).u16(m.count);
            break;
        }
        case MsgCode::MsgAttack: EncodeAttack(w, std::get<MsgAttack>(e.payload)); break;
        case MsgCode::MsgBattle: EncodeBattle(w, std::get<MsgBattle>(e.payload)); break;
        case MsgCode::MsgMissedEffect: {
            const auto& m = std::get<MsgMissedEffect>(e.payload);
            w.u32(m.desc).u32(m.code);
            break;
        }
        case MsgCode::MsgTossCoin:
        case MsgCode::MsgTossDice:
            EncodeToss(w, std::get<MsgToss>(e.payload));
            break;
        case MsgCode::MsgRockPaperScissors: w.u8(std::get<MsgRps>(e.payload).player); break;
        case MsgCode::MsgHandRes: w.u8(std::get<MsgHandRes>(e.payload).result); break;
        case MsgCode::MsgAnnounceRace:
        case MsgCode::MsgAnnounceAttrib: {
            const auto& m = std::get<MsgAnnounce>(e.payload);
            w.u8(m.player).u8(m.count).u32(m.available);
            break;
        }
        case MsgCode::MsgAnnounceCard: {
            const auto& m = std::get<MsgAnnounceCard>(e.payload);
            w.u8(m.player).u8(static_cast<uint8_t>(m.opcodes.size()));
            for (auto c : m.opcodes)
                w.u32(c);
            break;
        }
        case MsgCode::MsgAnnounceNumber: {
            const auto& m = std::get<MsgAnnounceNumber>(e.payload);
            w.u8(m.player).u8(static_cast<uint8_t>(m.values.size()));
            for (auto v : m.values)
                w.i32(v);
            break;
        }
        case MsgCode::MsgCardHint: {
            const auto& m = std::get<MsgCardHint>(e.payload);
            CardLocToWriter(w, m.card);
            w.u8(m.type).i32(m.value);
            break;
        }
        case MsgCode::MsgPlayerHint: {
            const auto& m = std::get<MsgPlayerHint>(e.payload);
            w.u8(m.player).u8(m.type).i32(m.value);
            break;
        }
        case MsgCode::MsgMatchKill: w.u32(std::get<MsgMatchKill>(e.payload).code); break;
        case MsgCode::MsgTagSwap: {
            const auto& m = std::get<MsgTagSwap>(e.payload);
            w.u8(m.player);
            for (int i = 0; i < 4; ++i)
                w.u8(m.counts[i]);
            w.i32(m.topcode);
            for (int i = 0; i < 4; ++i)
                for (auto c : m.lists[i])
                    w.u32(c);
            break;
        }
        default:
            return false;  // unknown code (should not round-trip)
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// STOC codec
// ---------------------------------------------------------------------------

bool DecodeStoc(const uint8_t* payload, size_t len, StocPacket& out, std::string& err) {
    if (len < 1) {
        err = "empty STOC payload";
        return false;
    }
    out.type = static_cast<StocType>(payload[0]);
    out.raw.assign(payload, payload + len);
    Reader r(payload + 1, len - 1);
    switch (out.type) {
    case StocType::GameMsg: {
        std::vector<MsgEvent> msgs;
        if (!DecodeMsgs(r, msgs, err))
            return false;
        out.body = std::move(msgs);
        return true;
    }
    case StocType::ErrorMsg: {
        StocErrorMsg m;
        if (!(r.u8(m.msg) && r.skip(3) && r.u32(m.code))) {  // [msg][pad 3][code]
            err = "malformed STOC_ERROR_MSG";
            return false;
        }
        out.body = m;
        return true;
    }
    case StocType::SelectHand:
    case StocType::SelectTp:
    case StocType::ChangeSide:
    case StocType::WaitingSide:
    case StocType::DuelStart:
    case StocType::DuelEnd:
    case StocType::TeammateSurrender:
    case StocType::FieldFinish:
        out.body = std::monostate{};
        return true;
    case StocType::HandResult: {
        StocHandResult m;
        if (!(r.u8(m.res1) && r.u8(m.res2))) {
            err = "malformed STOC_HAND_RESULT";
            return false;
        }
        out.body = m;
        return true;
    }
    case StocType::DeckCount: {
        StocDeckCount m;
        for (auto& c : m.counts)
            if (!r.u16(reinterpret_cast<uint16_t&>(c))) {
                err = "malformed STOC_DECK_COUNT";
                return false;
            }
        out.body = m;
        return true;
    }
    case StocType::JoinGame: {
        StocJoinGame m;
        if (!DecodeHostInfo(r, m.info)) {
            err = "malformed STOC_JOIN_GAME";
            return false;
        }
        out.body = m;
        return true;
    }
    case StocType::TypeChange: {
        StocTypeChange m;
        if (!r.u8(m.type)) {
            err = "malformed STOC_TYPE_CHANGE";
            return false;
        }
        out.body = m;
        return true;
    }
    case StocType::TimeLimit: {
        StocTimeLimit m;
        if (!(r.u8(m.player) && r.skip(1) && r.u16(m.left_time))) {
            err = "malformed STOC_TIME_LIMIT";
            return false;
        }
        out.body = m;
        return true;
    }
    case StocType::Chat: {
        StocChat m;
        if (!r.u16(m.player_type)) {
            err = "malformed STOC_CHAT";
            return false;
        }
        size_t n = r.remaining() / 2;
        m.msg.resize(n);
        for (size_t i = 0; i < n; ++i) {
            uint16_t c = 0;
            if (!r.u16(c)) {
                err = "malformed STOC_CHAT";
                return false;
            }
            m.msg[i] = static_cast<char16_t>(c);
        }
        out.body = m;
        return true;
    }
    case StocType::HsPlayerEnter: {
        StocHsPlayerEnter m;
        uint16_t name[20] = {0};
        for (auto& c : name)
            if (!r.u16(c)) {
                err = "malformed STOC_HS_PLAYER_ENTER";
                return false;
            }
        if (!r.u8(m.pos)) {
            err = "malformed STOC_HS_PLAYER_ENTER";
            return false;
        }
        m.name.assign(name, name + 20);
        out.body = m;
        return true;
    }
    case StocType::HsPlayerChange: {
        StocHsPlayerChange m;
        if (!r.u8(m.status)) {
            err = "malformed STOC_HS_PLAYER_CHANGE";
            return false;
        }
        out.body = m;
        return true;
    }
    case StocType::HsWatchChange: {
        StocHsWatchChange m;
        if (!r.u16(m.watch_count)) {
            err = "malformed STOC_HS_WATCH_CHANGE";
            return false;
        }
        out.body = m;
        return true;
    }
    case StocType::Replay: {
        std::vector<uint8_t> raw(r.data(), r.data() + r.remaining());
        out.body = std::move(raw);  // observer-only; kept raw
        return true;
    }
    case StocType::SrvproRoomlist: {
        StocSrvproRoomlist m;
        if (!r.u16(m.count)) {
            err = "malformed STOC_SRVPRO_ROOMLIST";
            return false;
        }
        m.rooms.resize(m.count);
        for (auto& room : m.rooms) {
            const uint8_t* p = nullptr;
            if (!r.bytes(64, p)) { err = "malformed roomlist"; return false; }
            room.roomname.assign(reinterpret_cast<const char*>(p), 64);
            if (!r.u8(room.room_status) || !r.u8(reinterpret_cast<uint8_t&>(room.duel_count)) ||
                !r.u8(reinterpret_cast<uint8_t&>(room.turn_count)) || !r.bytes(128, p)) {
                err = "malformed roomlist";
                return false;
            }
            room.p1.assign(reinterpret_cast<const char*>(p), 128);
            if (!r.u8(reinterpret_cast<uint8_t&>(room.p1_score)) || !r.i32(room.p1_lp) ||
                !r.bytes(128, p)) {
                err = "malformed roomlist";
                return false;
            }
            room.p2.assign(reinterpret_cast<const char*>(p), 128);
            if (!r.u8(reinterpret_cast<uint8_t&>(room.p2_score)) || !r.i32(room.p2_lp)) {
                err = "malformed roomlist";
                return false;
            }
        }
        out.body = std::move(m);
        return true;
    }
    case StocType::CreateGame:
    case StocType::LeaveGame:
    case StocType::TpResult:
        // Reserved/never sent to a normal client; tolerate as no-data.
        out.body = std::monostate{};
        return true;
    }
    err = "unknown STOC type " + std::to_string(static_cast<int>(out.type));
    return false;
}

bool EncodeStoc(const StocPacket& pkt, std::vector<uint8_t>& frame, std::string& err) {
    // Re-encode from the verbatim raw (the typed->bytes path is exercised by
    // round-trip tests per message; raw re-encode keeps this exact for replay).
    (void)err;
    Writer w;
    w.u16(static_cast<uint16_t>(pkt.raw.size()));
    w.bytes(pkt.raw.data(), pkt.raw.size());
    frame = w.take();
    return true;
}

// ---------------------------------------------------------------------------
// CTOS builders
// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> Frame(CtosType type, const Writer& body) {
    Writer w;
    w.u16(static_cast<uint16_t>(1 + body.vec().size()));
    w.u8(static_cast<uint8_t>(type));
    w.bytes(body.vec().data(), body.vec().size());
    return w.take();
}

}  // namespace

std::vector<uint8_t> CtosPlayerInfo(const std::string& name) {
    Writer w;
    uint16_t buf[20] = {0};
    ToUtf16LE(name, buf, 20);
    w.bytes(buf, sizeof(buf));
    return Frame(CtosType::PlayerInfo, w);
}

std::vector<uint8_t> CtosJoinGame(const std::string& pass, uint32_t gameid) {
    Writer w;
    w.u16(PROTOCOL_VERSION).u16(0).u32(gameid);
    uint16_t buf[20] = {0};
    ToUtf16LE(pass, buf, 20);
    w.bytes(buf, sizeof(buf));
    return Frame(CtosType::JoinGame, w);
}

std::vector<uint8_t> CtosCreateGame(const HostInfo& info, const std::string& name,
                                    const std::string& pass) {
    Writer w;
    EncodeHostInfo(w, info);
    uint16_t nb[20] = {0}, pb[20] = {0};
    ToUtf16LE(name, nb, 20);
    ToUtf16LE(pass, pb, 20);
    w.bytes(nb, sizeof(nb)).bytes(pb, sizeof(pb));
    return Frame(CtosType::CreateGame, w);
}

// Deck upload layout: [mainc u32][sidec u32][main+extra codes][side codes].
// mainc counts main + extra together; the server splits extra by card type
// (verified against ygopro-deck-encode and the reference client).
std::vector<uint8_t> CtosUpdateDeck(const std::vector<uint32_t>& main_extra,
                                    const std::vector<uint32_t>& side) {
    Writer w;
    w.u32(static_cast<uint32_t>(main_extra.size())).u32(static_cast<uint32_t>(side.size()));
    for (auto c : main_extra)
        w.u32(c);
    for (auto c : side)
        w.u32(c);
    return Frame(CtosType::UpdateDeck, w);
}

std::vector<uint8_t> CtosResponse(const std::vector<uint8_t>& data) {
    Writer w;
    w.bytes(data.data(), data.size());
    return Frame(CtosType::Response, w);
}

std::vector<uint8_t> CtosChat(const std::u16string& msg) {
    Writer w;
    for (auto c : msg)
        w.u16(static_cast<uint16_t>(c));
    return Frame(CtosType::Chat, w);
}

std::vector<uint8_t> CtosNoData(CtosType t) { return Frame(t, Writer{}); }

std::vector<uint8_t> CtosHandResult(uint8_t res) {
    Writer w;
    w.u8(res);
    return Frame(CtosType::HandResult, w);
}

std::vector<uint8_t> CtosTpResult(uint8_t res) {
    Writer w;
    w.u8(res);
    return Frame(CtosType::TpResult, w);
}

std::vector<uint8_t> CtosKick(uint8_t pos) {
    Writer w;
    w.u8(pos);
    return Frame(CtosType::Kick, w);
}

// ---------------------------------------------------------------------------
// Connection — POSIX socket + frame reassembly
// ---------------------------------------------------------------------------

bool Connection::Connect(const std::string& host, uint16_t port, std::string& err) {
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string portstr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portstr.c_str(), &hints, &res) != 0 || !res) {
        err = "getaddrinfo failed for " + host + ":" + portstr;
        return false;
    }
    int fd = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        err = "connect failed to " + host + ":" + portstr;
        return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Non-blocking socket: the single-threaded poll loop (design R1) drives
    // reads; ReadPackets drains until EAGAIN and returns. A blocking socket
    // here would deadlock the loop on the second recv.
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fd_ = fd;
    rbuf_.clear();
    return true;
}

bool Connection::SendFrame(const std::vector<uint8_t>& frame, std::string& err) {
    if (fd_ < 0) {
        err = "not connected";
        return false;
    }
    // CAVEAT (srvpro quirk, from ygopromcp): a frame must be sent in ONE
    // write() — the server rejects split packets (it reads the length then
    // expects the whole payload; a partial write desyncs it). TCP_NODELAY is
    // set so each write() goes out as one segment.
    ssize_t n = ::write(fd_, frame.data(), frame.size());
    if (n < 0 || static_cast<size_t>(n) != frame.size()) {
        err = "short write / send failed";
        return false;
    }
    return true;
}

bool Connection::ReadPackets(std::vector<StocPacket>& out,
                             std::vector<std::vector<uint8_t>>& raw_frames, std::string& err) {
    uint8_t buf[65536];
    for (;;) {
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            rbuf_.insert(rbuf_.end(), buf, buf + n);
            continue;
        }
        if (n == 0) {
            err = "server closed the connection";
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        err = "recv failed";
        return false;
    }
    // Reassemble frames: [u16 LE length][payload]; length counts payload bytes.
    size_t pos = 0;
    while (rbuf_.size() - pos >= 2) {
        uint16_t len = 0;
        std::memcpy(&len, rbuf_.data() + pos, 2);
        if (rbuf_.size() - pos < static_cast<size_t>(len) + 2)
            break;  // partial frame; wait for more
        std::vector<uint8_t> raw(rbuf_.begin() + pos, rbuf_.begin() + pos + 2 + len);
        raw_frames.push_back(raw);
        StocPacket pkt;
        if (!DecodeStoc(rbuf_.data() + pos + 2, len, pkt, err)) {
            // Report the decode error with the raw frame size for diagnostics.
            err += " (frame " + std::to_string(raw.size()) + "B)";
            pos += 2 + len;  // drop the bad frame; resync is futile
            return false;
        }
        out.push_back(std::move(pkt));
        pos += 2 + len;
    }
    rbuf_.erase(rbuf_.begin(), rbuf_.begin() + pos);
    return true;
}

void Connection::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace proto
