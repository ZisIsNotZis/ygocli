// main.cpp — arg parsing + the single-threaded poll event loop (design R1).
//
// One thread, two fds: the game socket and stdin (JSON-RPC). Socket readable ->
// game.ReadAndDispatch (decode + state machine + auto-responses for RPS/TP/
// time-confirm). Stdin readable -> one JSON-RPC line -> mcp dispatch (tool
// handlers call into game). Prompt-waiting is just state; no locks, no races.
//
// The random-response driver is GONE: ygo_choose (a registered tool) is the
// interaction surface, and the game layer encodes valid responses. The e2e
// random test drives two of these via stdio JSON-RPC.

#include "game.h"
#include "mcp.h"
#include "proto.h"

#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

namespace {

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 7911;
    std::string room = "ygomcp";
    std::string name = "ygomcp";
    std::string deckfile;
    std::string replayfile;  // --replay: decode a capture, print the stream
    std::string carddb = "cards.cdb";
};

void Usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s -d deck.ydk [-h host] [-p port] [-w room] [-n name] [--carddb path]\n"
        "       %s --replay capture.bin\n"
        "  headless MCP duel client: connects to the server, joins the room, and\n"
        "  speaks JSON-RPC over stdio (initialize / tools/list / tools/call).\n",
        argv0, argv0);
}

bool ParseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                return false;
            }
            return true;
        };
        if (a == "-h" && next("host")) o.host = argv[++i];
        else if (a == "-p" && next("port")) o.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "-w" && next("room")) o.room = argv[++i];
        else if (a == "-n" && next("name")) o.name = argv[++i];
        else if (a == "-d" && next("deck")) o.deckfile = argv[++i];
        else if (a == "--replay" && next("file")) o.replayfile = argv[++i];
        else if (a == "--carddb" && next("path")) o.carddb = argv[++i];
        else if (a == "--help") { Usage(argv[0]); return false; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            Usage(argv[0]);
            return false;
        }
    }
    if (o.deckfile.empty() && o.replayfile.empty()) {
        std::fprintf(stderr, "missing -d deck.ydk (or --replay file)\n");
        Usage(argv[0]);
        return false;
    }
    return true;
}

// --replay: walk the captured frames and print the decoded MSG_* stream
// (fixture tool: a capture is the ground truth the game-layer tests replay).
int ReplayFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open replay: %s\n", path.c_str());
        return 1;
    }
    int frames = 0, msgs = 0;
    for (;;) {
        uint8_t hdr[2];
        f.read(reinterpret_cast<char*>(hdr), 2);
        if (f.gcount() < 2)
            break;
        uint16_t len = static_cast<uint16_t>(hdr[0]) | (static_cast<uint16_t>(hdr[1]) << 8);
        std::vector<uint8_t> payload(len);
        f.read(reinterpret_cast<char*>(payload.data()), len);
        if (f.gcount() < static_cast<std::streamsize>(len))
            break;
        ++frames;
        proto::StocPacket pkt;
        std::string err;
        if (!proto::DecodeStoc(payload.data(), len, pkt, err)) {
            std::printf("frame %d: DECODE ERROR: %s\n", frames, err.c_str());
            continue;
        }
        if (pkt.type != proto::StocType::GameMsg)
            continue;
        const auto& evs = std::get<std::vector<proto::MsgEvent>>(pkt.body);
        for (const auto& e : evs) {
            ++msgs;
            std::printf("  msg %d (%zuB)\n", e.code, e.raw.size());
        }
    }
    std::printf("frames=%d msgs=%d\n", frames, msgs);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Options opt;
    if (!ParseArgs(argc, argv, opt))
        return 2;
    if (!opt.replayfile.empty())
        return ReplayFile(opt.replayfile);

    // Game + card db.
    proto::Connection conn;
    game::Game game(conn);
    std::string err;
    if (!game.OpenCardDb(opt.carddb, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!game.Start(opt.host, opt.port, opt.room, opt.name, opt.deckfile, err)) {
        std::fprintf(stderr, "start: %s\n", err.c_str());
        return 1;
    }

    // MCP registry + tool registration (game owns the semantics; mcp stays
    // transport-only — the layering seam).
    mcp::Registry mcp;
    mcp.Register({"ygo_state", "current board + pending prompt",
                  {{"type", "object"}},
                  [&game](const mcp::Json& p) { return game.ToolState(p); }});
    mcp.Register({"ygo_log", "narration log (since: skip this many lines)",
                  {{"type", "object"}, {"properties", {{"since", {{"type", "integer"}}}}}},
                  [&game](const mcp::Json& p) { return game.ToolLog(p); }});
    mcp.Register({"ygo_card", "card database lookup by name or code",
                  {{"type", "object"},
                   {"properties",
                    {{"name", {{"type", "string"}}}, {"code", {{"type", "string"}}}}}},
                  [&game](const mcp::Json& p) { return game.ToolCard(p); }});
    mcp.Register({"ygo_choose", "answer the pending prompt (id: N, or indices: [..])",
                  {{"type", "object"},
                   {"properties",
                    {{"id", {{"type", "integer"}}},
                     {"indices", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
                     {"cancel", {{"type", "boolean"}}}}}},
                  [&game](const mcp::Json& p) { return game.ToolChoose(p); }});
    mcp.Register({"ygo_chat", "send a chat message",
                  {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
                  [&game](const mcp::Json& p) { return game.ToolChat(p); }});
    mcp.Register({"ygo_chatlog", "received chat messages",
                  {{"type", "object"}},
                  [&game](const mcp::Json& p) { return game.ToolChatlog(p); }});

    // The single-threaded poll loop: {game socket, stdin}.
    std::string linebuf;
    for (;;) {
        struct pollfd fds[2];
        fds[0] = {conn.fd(), POLLIN, 0};
        fds[1] = {STDIN_FILENO, POLLIN, 0};
        int rc = poll(fds, 2, 1000);
        if (rc < 0) {
            std::fprintf(stderr, "poll failed\n");
            return 1;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (!game.ReadAndDispatch(err)) {
                // Server closed or decode failure; report and exit.
                std::fprintf(stderr, "connection ended: %s\n", err.c_str());
                return 0;
            }
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                if (std::cin.eof())
                    return 0;  // stdin closed (the driver harness went away)
                continue;
            }
            if (line.empty())
                continue;
            mcp::Json req = mcp::Json::parse(line, nullptr, false);
            if (req.is_discarded()) {
                std::printf("%s\n", mcp::Json{{"jsonrpc", "2.0"}, {"id", nullptr},
                                              {"error", {{"code", -32700}, {"message", "parse error"}}}}
                                         .dump()
                                         .c_str());
                continue;
            }
            mcp::Json resp = mcp.Handle(req);
            if (!resp.is_null())
                std::cout << resp.dump() << "\n";
        }
    }
    return 0;
}
