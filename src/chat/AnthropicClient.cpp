// Claude through Anthropic's Messages API (POST /v1/messages, streaming server-sent events), over raw HTTP:
// there is no official Anthropic SDK for C++. The chat keeps its history in the OpenAI shape; this file turns it
// into Messages API content blocks and the streamed answer back into the shared StreamResult.
#include "LlmClient.h"

#include "Util.h"

#include <algorithm>
#include <chrono>
#include <regex>

namespace chat
{

namespace
{

    // Claude 4.6 and later (and every 5.x model) take adaptive thinking; older models would need a token budget.
bool AdaptiveThinking(const std::string& model)
{
    static const std::regex re(R"(claude-(opus|sonnet|haiku|fable|mythos)-(\d+)(?:-(\d+))?)");
    std::smatch m;
    if (!std::regex_search(model, m, re))
    {
        return false;
    }

    int major = std::stoi(m[2].str()), minor = m[3].matched ? std::stoi(m[3].str()) : 0;
    if (major >= 5)
    {
        return true;
    }

    return major == 4 && minor >= 6 && m[1].str() != "haiku";
}

    // Models whose refusals the API can hand to another model in the same call (server-side fallbacks).
bool ServerFallbacks(const std::string& model)
{
    for (const char* id : { "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5-5", "claude-fable-5-2", "claude-fable-5-1" })
    {
        if (model == id)
        {
            return true;
        }
    }

    return false;
}

int MaxTokensFor(const std::string& model)
{
        // 64K leaves room for long answers on current models (they allow up to 128K when streaming).
    return AdaptiveThinking(model) || model.find("4-5") != std::string::npos ? 64000 : 8192;
}

std::string Text(const json& content)
{
    if (content.is_string())
    {
        return content.get<std::string>();
    }

    std::string out;
    if (content.is_array())
    {
        for (const auto& part : content)
        {
            if (part.is_object() && part.value("type", "") == "text")
            {
                out += part.value("text", "");
            }
        }
    }

    return out;
}

    // "data:image/png;base64,AAAA" -> an image block; null for formats Claude does not read.
json ImageBlock(const std::string& url)
{
    if (url.rfind("data:", 0) != 0)
    {
        return { { "type", "image" }, { "source", { { "type", "url" }, { "url", url } } } };
    }

    size_t semi = url.find(';'), comma = url.find(',');
    if (semi == std::string::npos || comma == std::string::npos)
    {
        return json();
    }

    std::string mime = url.substr(5, semi - 5);
    if (mime != "image/png" && mime != "image/jpeg" && mime != "image/gif" && mime != "image/webp")
    {
        return json();
    }

    return { { "type", "image" }, { "source", { { "type", "base64" }, { "media_type", mime }, { "data", url.substr(comma + 1) } } } };
}

    // The OpenAI-shaped history -> Messages API messages (roles alternate; tool results go back in one user turn).
json ToMessages(const json& history, std::string& system)
{
    json out = json::array();
    auto push = [&](const char* role, json blocks)
    {
        if (blocks.empty())
        {
            return;
        }

        if (!out.empty() && out.back()["role"] == role)
        {
            for (auto& b : blocks)
            {
                out.back()["content"].push_back(b);
            }
        }
        else
        {
            out.push_back({ { "role", role }, { "content", blocks } });
        }
    };

    for (const auto& m : history)
    {
        std::string role = m.value("role", "");
        if (role == "system")
        {
            std::string t = Text(m.value("content", json("")));
            if (!t.empty())
            {
                system += (system.empty() ? "" : "\n\n") + t;
            }
        }
        else if (role == "user")
        {
            json blocks = json::array();
            const json& c = m.contains("content") ? m["content"] : json("");
            if (c.is_array())
            {
                for (const auto& part : c)
                {
                    std::string type = part.value("type", "");
                    if (type == "text" && !part.value("text", "").empty())
                    {
                        blocks.push_back({ { "type", "text" }, { "text", part.value("text", "") } });
                    }
                    else if (type == "image_url")
                    {
                        json img = ImageBlock(part["image_url"].value("url", ""));
                        blocks.push_back(img.is_null() ? json{ { "type", "text" }, { "text", "[an image in a format Claude does not read]" } } : img);
                    }
                }
            }
            else
            {
                std::string t = Text(c);
                blocks.push_back({ { "type", "text" }, { "text", t.empty() ? "(empty message)" : t } });
            }

            push("user", blocks);
        }
        else if (role == "assistant")
        {
            json blocks = json::array();
            if (m.contains("x_anthropic") && m["x_anthropic"].is_array() && !m["x_anthropic"].empty())
            {
                blocks = m["x_anthropic"];  // Claude's own blocks (thinking with signatures, tool_use), unchanged
            }
            else
            {
                std::string t = Text(m.value("content", json("")));
                if (!TrimCopy(t).empty())
                {
                    blocks.push_back({ { "type", "text" }, { "text", t } });
                }

                if (m.contains("tool_calls") && m["tool_calls"].is_array())
                {
                    for (const auto& tc : m["tool_calls"])
                    {
                        json input = json::parse(tc["function"].value("arguments", "{}"), nullptr, false);
                        blocks.push_back({ { "type", "tool_use" }, { "id", tc.value("id", "") },
                            { "name", tc["function"].value("name", "") },
                            { "input", input.is_object() ? input : json::object() } });
                    }
                }
            }

            if (blocks.empty())
            {
                blocks.push_back({ { "type", "text" }, { "text", "(no answer)" } });
            }

            push("assistant", blocks);
        }
        else if (role == "tool")
        {
            std::string t = Text(m.value("content", json("")));
            json result = { { "type", "tool_result" }, { "tool_use_id", m.value("tool_call_id", "") },
                { "content", t.empty() ? "(no output)" : t } };

            if (t.rfind("Error", 0) == 0)
            {
                result["is_error"] = true;
            }

            push("user", json::array({ result }));
        }
    }

    return out;
}

    // After a mid-answer fallback, the blocks before the last "fallback" block are echoed without thinking, tool_use
    // and other model-internal blocks; the fallback block itself stays where it is.
json EchoBlocks(const json& blocks)
{
    int lastFallback = -1;
    for (int i = 0; i < static_cast<int>(blocks.size()); ++i)
    {
        if (blocks[i].value("type", "") == "fallback")
        {
            lastFallback = i;
        }
    }

    if (lastFallback < 0)
    {
        return blocks;
    }

    json out = json::array();
    for (int i = 0; i < static_cast<int>(blocks.size()); ++i)
    {
        std::string type = blocks[i].value("type", "");
        bool keep = i >= lastFallback || type == "text";
        if (keep)
        {
            out.push_back(blocks[i]);
        }
    }

    return out;
}

}  // namespace

StreamResult StreamAnthropic(const Endpoint& ep, const json& request,
    const std::function<void(DeltaKind, const std::string&)>& onDelta, CancelToken* cancel)
{
    StreamResult res;
    std::string model = request.value("model", "");
    std::string system;
    json body = { { "model", model }, { "max_tokens", MaxTokensFor(model) }, { "stream", true },
        { "messages", ToMessages(request.value("messages", json::array()), system) } };

    if (!system.empty())
    {
        body["system"] = system;
    }

    if (request.contains("tools") && request["tools"].is_array() && !request["tools"].empty())
    {
        json tools = json::array();
        for (const auto& t : request["tools"])
        {
            const json& f = t["function"];
            json schema = f.contains("parameters") && f["parameters"].is_object() ? f["parameters"]
                                                                                  : json{ { "type", "object" } };
            if (!schema.contains("type"))
            {
                schema["type"] = "object";
            }

            // eager_input_streaming: large inputs (file contents) stream as they are written. The input is then
            // not validated by the server: RunJob checks that it parses as a JSON object before running a tool.
            tools.push_back({ { "name", f.value("name", "") }, { "description", f.value("description", "") },
                { "input_schema", schema }, { "eager_input_streaming", true } });
        }

        body["tools"] = tools;
    }
    // Reasoning: adaptive thinking with a readable summary; the "+" menu's level becomes the effort.
    // (Claude Opus 5.5 cannot turn thinking off: "Off" asks for low effort.)
    if (AdaptiveThinking(model))
    {
        body["thinking"] = { { "type", "adaptive" }, { "display", "summarized" } };
        std::string effort = request.value("reasoning_effort", "");
        bool off = request.contains("chat_template_kwargs") &&
                   request["chat_template_kwargs"].value("enable_thinking", true) == false;
        if (off)
        {
            effort = "low";
        }

        if (effort == "low" || effort == "medium" || effort == "high")
        {
            body["output_config"] = { { "effort", effort } };
        }
    }
    // temperature is not sent: API version 2026-10-21 rejects sampling parameters.

    HttpHeaders headers = ep.headers;
    headers.push_back({ "Content-Type", "application/json" });
    headers.push_back({ "Accept", "text/event-stream" });
    if (ServerFallbacks(model))
    {
        // If a safety classifier declines, the API retries on a model chosen for that kind of request.
        body["fallbacks"] = "default";

        // One anthropic-beta header, comma-separated (the OAuth beta may be there already).
        auto beta = std::find_if(headers.begin(), headers.end(), [](const auto& h)
            { return h.first == "anthropic-beta"; });

        if (beta != headers.end())
        {
            beta->second += ",server-side-fallback-2026-07-01";
        }
        else
        {
            headers.push_back({ "anthropic-beta", "server-side-fallback-2026-07-01" });
        }
    }

    std::map<int, json> blocks;
    std::map<int, std::string> partialJson;
    std::string stopReason, refusalCategory;
    int outputTokens = 0;
    auto started = std::chrono::steady_clock::now();
    bool sawOutput = false;
    auto firstOutput = started;

    SseParser sse;
    auto onEvent = [&](const std::string&, const std::string& data) -> bool
    {
        json e = json::parse(data, nullptr, false);
        if (!e.is_object())
        {
            return true;
        }

        std::string type = e.value("type", "");
        if (type == "message_start")
        {
            res.servedModel = e["message"].value("model", model);
        }
        else if (type == "content_block_start")
        {
            int i = e.value("index", 0);
            blocks[i] = e["content_block"];
            if (blocks[i].value("type", "") == "tool_use")
            {
                partialJson[i].clear();
            }
        }
        else if (type == "content_block_delta")
        {
            int i = e.value("index", 0);
            const json& d = e["delta"];
            std::string dt = d.value("type", "");
            if (!sawOutput)
            {
                sawOutput = true;
                firstOutput = std::chrono::steady_clock::now();
            }

            if (dt == "text_delta")
            {
                std::string t = d.value("text", "");
                blocks[i]["text"] = blocks[i].value("text", "") + t;
                res.content += t;
                onDelta(DeltaKind::Content, t);
            }
            else if (dt == "thinking_delta")
            {
                std::string t = d.value("thinking", "");
                blocks[i]["thinking"] = blocks[i].value("thinking", "") + t;
                res.reasoning += t;
                onDelta(DeltaKind::Reasoning, t);
            }
            else if (dt == "signature_delta")
            {
                blocks[i]["signature"] = d.value("signature", "");
            }
            else if (dt == "input_json_delta")
            {
                partialJson[i] += d.value("partial_json", "");
            }
        }
        else if (type == "content_block_stop")
        {
            int i = e.value("index", 0);
            if (blocks[i].value("type", "") == "tool_use")
            {
                json input = json::parse(partialJson[i].empty() ? "{}" : partialJson[i], nullptr, false);
                blocks[i]["input"] = input.is_object() ? input : json::object();
            }
        }
        else if (type == "message_delta")
        {
            if (e.contains("delta"))
            {
                stopReason = e["delta"].value("stop_reason", stopReason);
                if (e["delta"].contains("stop_details") && e["delta"]["stop_details"].is_object())
                {
                    const json& sd = e["delta"]["stop_details"];
                    refusalCategory = sd.contains("category") && sd["category"].is_string() ? sd["category"].get<std::string>() : "";
                }
            }

            if (e.contains("usage") && e["usage"].is_object())
            {
                outputTokens = e["usage"].value("output_tokens", outputTokens);
            }
        }
        else if (type == "error")
        {
            res.error = e.contains("error") ? e["error"].value("message", e.dump()) : e.dump();
            return false;
        }

        return true;  // ping, message_stop
    };

    HttpResult r = HttpSend(L"POST", ep.baseUrl + "/messages", body.dump(-1, ' ', false, json::error_handler_t::replace), headers, 30 * 60 * 1000, [&](const char* data, size_t size)
        { return sse.Feed(data, size, onEvent); }, cancel);

    if (r.Ok() && res.error.empty())
    {
        sse.Finish(onEvent);
    }

    if (r.cancelled && cancel && cancel->IsCancelled())
    {
        res.cancelled = true;
    }

    if (!r.Ok() && !res.cancelled)
    {
        json j = json::parse(r.body, nullptr, false);
        std::string msg = j.is_object() && j.contains("error") ? j["error"].value("message", r.body) : r.body;
        res.error = r.status == 0     ? r.error
                    : r.status == 401 ? "Claude: the API key was not accepted"
                                      : "Claude: HTTP " + std::to_string(r.status) + ": " + Truncate(msg, 400);
        return res;
    }

    json raw = json::array();
    for (auto& [i, b] : blocks)
    {
        raw.push_back(b);
    }

    res.providerContent = EchoBlocks(raw);
    res.finishReason = stopReason;
    res.completionTokens = outputTokens;

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - firstOutput).count();

    if (sawOutput && secs > 0.2 && outputTokens > 0)
    {
        res.tokensPerSecond = outputTokens / secs;
    }

    if (stopReason == "refusal")
    {
        res.error = "Claude declined this request" + (refusalCategory.empty() ? std::string() : " (" + refusalCategory + ")");
        return res;
    }

    // Tool calls of this answer: the tool_use blocks after the last fallback switch.
    for (const auto& b : res.providerContent)
    {
        if (b.value("type", "") == "tool_use")
        {
            res.toolCalls.push_back({ { "id", b.value("id", "") }, { "type", "function" },
                { "function", { { "name", b.value("name", "") }, { "arguments", b["input"].dump() } } } });
        }
    }

    if (stopReason == "max_tokens" && !res.toolCalls.empty())
    {
        res.error = "the answer reached its length limit in the middle of a tool call";
        res.toolCalls = json::array();
    }

    return res;
}

}  // namespace chat
