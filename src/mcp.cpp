// mcp.cpp — JSON-RPC transport implementation (see mcp.h).

#include "mcp.h"

namespace mcp {

void Registry::Register(Tool tool) {
    tools_[tool.name] = std::move(tool);
}

Json Registry::Error(int code, const std::string& msg, Json id) {
    return Json{{"jsonrpc", "2.0"}, {"id", std::move(id)},
                {"error", {{"code", code}, {"message", msg}}}};
}

Json Registry::Ok(Json id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"result", std::move(result)}};
}

Json Registry::HandleCall(const Json& req) {
    const Json& params = req.contains("params") && req["params"].is_object()
                             ? req["params"]
                             : Json::object();
    const std::string name = params.value("name", "");
    Json id = req.contains("id") ? req["id"] : Json(nullptr);
    auto it = tools_.find(name);
    if (it == tools_.end())
        return Error(-32602, "unknown tool: " + name, id);
    Json arguments = params.contains("arguments") ? params["arguments"] : Json::object();
    try {
        Json text = it->second.handler(arguments);
        Json content = Json::array();
        if (text.is_string()) {
            content.push_back({{"type", "text"}, {"text", text.get<std::string>()}});
        } else if (text.is_array()) {
            content = text;  // pre-built content array
        } else {
            content.push_back({{"type", "text"},
                               {"text", text.is_object() ? text.dump() : text.dump()}});
        }
        return Ok(std::move(id), {{"content", std::move(content)}});
    } catch (const std::exception& e) {
        // Tool handler raised: surface as a JSON-RPC error (the caller sees
        // isError, not a crash).
        return Error(-32000, std::string("tool failed: ") + e.what(), id);
    }
}

Json Registry::HandleList(const Json& req) {
    Json id = req.contains("id") ? req["id"] : Json(nullptr);
    Json tools = Json::array();
    for (const auto& [name, tool] : tools_) {
        tools.push_back({{"name", name},
                         {"description", tool.description},
                         {"inputSchema", tool.input_schema}});
    }
    return Ok(std::move(id), {{"tools", std::move(tools)}});
}

Json Registry::Handle(const Json& req) {
    if (!req.is_object() || req.value("jsonrpc", "") != "2.0")
        return Error(-32600, "invalid request", nullptr);
    const std::string method = req.value("method", "");
    Json id = req.contains("id") ? req["id"] : Json(nullptr);
    if (method == "initialize") {
        // Capabilities: tools only (no resources/prompts). Protocol version
        // echoed back per the MCP spec.
        return Ok(std::move(id),
                  {{"protocolVersion", req["params"].value("protocolVersion", "2024-11-05")},
                   {"capabilities", {{"tools", {{"listChanged", false}}}}},
                   {"serverInfo", {{"name", "ygomcp"}, {"version", "0.1.0"}}}});
    }
    if (method == "tools/list")
        return HandleList(req);
    if (method == "tools/call")
        return HandleCall(req);
    if (method == "notifications/initialized")
        return Json();  // notification: no response
    return Error(-32601, "method not found: " + method, std::move(id));
}

}  // namespace mcp
