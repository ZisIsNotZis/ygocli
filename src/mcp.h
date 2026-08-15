// mcp.h — pure JSON-RPC 2.0 transport + tool registry (M4).
//
// Layering (per design): mcp knows NOTHING about ygo, the duel, or the wire
// protocol. game registers tool handlers (name -> {description, schema, fn});
// mcp serves initialize / tools/list / tools/call and dispatches invocations.
// This makes the transport independently testable with a fake registry.
//
// Framing: newline-delimited JSON-RPC over stdio (one JSON object per line) —
// the same convention as the ygopromcp client, so existing tooling/tests
// transfer 1:1. initialize handshake per the MCP protocol version we speak.

#pragma once

#include <functional>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace mcp {

using Json = nlohmann::json;

struct Tool {
    std::string name;
    std::string description;
    Json input_schema;                                  // JSON Schema for parameters
    std::function<Json(const Json& params)> handler;    // returns the text result
};

class Registry {
public:
    void Register(Tool tool);
    const std::map<std::string, Tool>& Tools() const { return tools_; }

    // Handles one JSON-RPC request line; returns the response JSON (or nullopt
    // for notifications). Malformed input yields a JSON-RPC error response.
    Json Handle(const Json& req);

private:
    std::map<std::string, Tool> tools_;
    Json HandleCall(const Json& req);
    Json HandleList(const Json& req);
    Json Error(int code, const std::string& msg, Json id);
    Json Ok(Json id, Json result);
};

}  // namespace mcp
