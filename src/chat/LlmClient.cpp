#include "LlmClient.h"

#include "Codex.h"
#include "Copilot.h"
#include "Util.h"

#include "modelhub/modelhub_client.h"

#include <algorithm>

namespace chat
{

// The client starts ModelHub.exe from install.json, MODELHUB_EXE or PATH. A ModelHub.exe next to this program
// (the build output and the release zip) is the last resort: it goes into MODELHUB_EXE for this process only.
static void OfferModelHubNextToUs()
{
    wchar_t buf[8];
    if (GetEnvironmentVariableW(L"MODELHUB_EXE", buf, 8) > 0)
    {
        return;
    }

    std::wstring exe = ExeDir() + L"\\ModelHub.exe";
    if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        SetEnvironmentVariableW(L"MODELHUB_EXE", exe.c_str());
    }
}

bool ResolveEndpoint(const Settings& settings, Endpoint& ep, std::string& error,
    const std::function<void(const std::string&)>& onStarting, bool* started)
{
    if (started)
    {
        *started = false;
    }

    ep = Endpoint{};
    if (settings.backend == "custom")
    {
        ep.baseUrl = TrimCopy(settings.customUrl);
        while (!ep.baseUrl.empty() && ep.baseUrl.back() == '/')
        {
            ep.baseUrl.pop_back();
        }

        if (ep.baseUrl.empty())
        {
            error = "no server URL: set one in Settings";
            return false;
        }

        if (!settings.customKey.empty())
        {
            ep.headers.push_back({ "Authorization", "Bearer " + settings.customKey });
        }

        ep.label = ep.baseUrl;
        return true;
    }

    modelhub::client::Client hub("ModelHubChat");
    if (!hub.Connect(false, 0, nullptr))
    {
        if (!settings.autoStartHub)
        {
            error = "ModelHub is not running (start it from ModelHub Manager, or turn on \"Start ModelHub when "
                    "needed\" in Settings)";
            return false;
        }

        OfferModelHubNextToUs();
        if (onStarting)
        {
            onStarting("Starting ModelHub...");
        }

        if (!hub.Connect(true, 30000, &error))
        {
            return false;
        }

        if (started)
        {
            *started = true;
        }
    }

    ep.baseUrl = hub.BaseUrl();
    ep.headers.push_back({ "Authorization", "Bearer " + hub.Token() });
    ep.headers.push_back({ "X-ModelHub-App", "ModelHubChat" });
    ep.label = "ModelHub 127.0.0.1:" + std::to_string(hub.Port());
    ep.modelhub = true;
    return true;
}

static std::string ErrorText(const HttpResult& r)
{
    if (r.status == 0)
    {
        return r.error.empty() ? "no response" : r.error;
    }

    json j = json::parse(r.body, nullptr, false);
    if (j.is_array() && !j.empty())
    {
        j = j[0];  // Gemini wraps its error in a list
    }

    if (j.is_object() && j.contains("error"))
    {
        const json& e = j["error"];
        if (e.is_object() && e.contains("message") && e["message"].is_string())
        {
            return "HTTP " + std::to_string(r.status) + ": " + e["message"].get<std::string>();
        }

        if (e.is_string())
        {
            return "HTTP " + std::to_string(r.status) + ": " + e.get<std::string>();
        }
    }

    return "HTTP " + std::to_string(r.status) + ": " + Truncate(r.body, 500);
}

bool ListModels(const Endpoint& ep, std::vector<std::string>& ids, std::string& error)
{
    ids.clear();
    HttpResult r = HttpSend(L"GET", ep.baseUrl + "/models", "", ep.headers, 15000);
    if (!r.Ok())
    {
        error = ErrorText(r);
        return false;
    }

    json j = json::parse(r.body, nullptr, false);
    const json* list = nullptr;
    if (j.is_object() && j.contains("data") && j["data"].is_array())
    {
        list = &j["data"];
    }
    else if (j.is_object() && j.contains("models") && j["models"].is_array())
    {
        list = &j["models"];
    }

    if (list)
    {
        for (const auto& m : *list)
        {
            if (m.is_object())
            {
                std::string id = m.value("id", m.value("model", m.value("name", "")));
                if (!id.empty())
                {
                    ids.push_back(id);
                }
            }
        }
    }

    if (ep.modelhub)
    {
        // ModelHub knows which models only make embeddings: they cannot chat.
        std::string root = ep.baseUrl.substr(0, ep.baseUrl.size() - 3);  // without "/v1"
        HttpResult d = HttpSend(L"GET", root + "/hub/v1/models", "", ep.headers, 15000);

        json details = json::parse(d.Ok() ? d.body : std::string("{}"), nullptr, false);
        if (details.is_object() && details.contains("models") && details["models"].is_array())
        {
            std::set<std::string> embeddings;
            for (const auto& m : details["models"])
            {
                if (m.is_object() && m.value("embeddings", false))
                {
                    embeddings.insert(m.value("id", ""));
                }
            }

            ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const std::string& id)
                          { return embeddings.count(id) > 0; }),
                ids.end());
        }
    }

    return true;
}

namespace
{

// Splits <think>...</think> written into the content (servers that do not separate reasoning themselves).
class ThinkSplitter
{
public:
    void Content(const std::string& delta, const std::function<void(DeltaKind, const std::string&)>& out)
    {
        std::string s = pending_ + delta;
        pending_.clear();
        while (!s.empty())
        {
            const std::string tag = inThink_ ? "</think>" : "<think>";
            size_t at = s.find(tag);
            if (at == std::string::npos)
            {
                // Hold back a possible start of the tag.
                size_t keep = 0;
                for (size_t k = std::min(tag.size() - 1, s.size()); k > 0; --k)
                {
                    if (s.compare(s.size() - k, k, tag, 0, k) == 0)
                    {
                        keep = k;
                        break;
                    }
                }

                Emit(s.substr(0, s.size() - keep), out);
                pending_ = s.substr(s.size() - keep);
                return;
            }

            if (!inThink_ && (sawContent_ || TrimCopy(s.substr(0, at)).size() > 0))
            {
                Emit(s.substr(0, at + tag.size()), out);  // a literal "<think>" later in an answer
                s.erase(0, at + tag.size());
                continue;
            }

            Emit(s.substr(0, at), out);
            s.erase(0, at + tag.size());
            inThink_ = !inThink_;
        }
    }
    void Flush(const std::function<void(DeltaKind, const std::string&)>& out)
    {
        Emit(pending_, out);
        pending_.clear();
    }

private:
    void Emit(const std::string& text, const std::function<void(DeltaKind, const std::string&)>& out)
    {
        if (text.empty())
        {
            return;
        }

        if (!inThink_)
        {
            sawContent_ = sawContent_ || !TrimCopy(text).empty();
        }

        out(inThink_ ? DeltaKind::Reasoning : DeltaKind::Content, text);
    }

    bool inThink_ = false, sawContent_ = false;
    std::string pending_;
};

}  // namespace

StreamResult StreamChat(const Endpoint& ep, const json& request,
    const std::function<void(DeltaKind, const std::string&)>& onDelta, CancelToken* cancel, const ToolRunner& runTool)
{
    if (ep.kind == "anthropic")
    {
        return StreamAnthropic(ep, request, onDelta, cancel);
    }

    if (ep.kind == "copilot")
    {
        return StreamCopilot(ep, request, onDelta, cancel, runTool);
    }

    if (ep.kind == "codex")
    {
        return StreamCodex(ep, request, onDelta, cancel);
    }

    StreamResult res;
    std::map<int, json> calls;  // index -> {id, type, function:{name, arguments}}
    ThinkSplitter splitter;
    auto emit = [&](DeltaKind kind, const std::string& text)
    {
        (kind == DeltaKind::Reasoning ? res.reasoning : res.content) += text;
        onDelta(kind, text);
    };

    SseParser sse;
    bool done = false;
    auto onEvent = [&](const std::string&, const std::string& data) -> bool
    {
        if (data == "[DONE]")
        {
            done = true;
            return false;
        }

        json j = json::parse(data, nullptr, false);
        if (!j.is_object())
        {
            return true;
        }

        if (j.contains("error"))
        {
            const json& e = j["error"];
            res.error = e.is_object() ? e.value("message", e.dump()) : e.is_string() ? e.get<std::string>()
                                                                                     : e.dump();
            return false;
        }

        if (j.contains("timings") && j["timings"].is_object())
        {
            res.tokensPerSecond = j["timings"].value("predicted_per_second", 0.0);
            res.completionTokens = j["timings"].value("predicted_n", 0);
        }

        if (j.contains("usage") && j["usage"].is_object())
        {
            res.completionTokens = j["usage"].value("completion_tokens", res.completionTokens);
        }

        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
        {
            return true;
        }

        const json& choice = j["choices"][0];
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
        {
            res.finishReason = choice["finish_reason"].get<std::string>();
        }

        if (!choice.contains("delta") || !choice["delta"].is_object())
        {
            return true;
        }

        const json& d = choice["delta"];
        for (const char* key : { "reasoning_content", "reasoning" })
        {
            if (d.contains(key) && d[key].is_string() && !d[key].get<std::string>().empty())
            {
                emit(DeltaKind::Reasoning, d[key].get<std::string>());
            }
        }

        if (d.contains("content") && d["content"].is_string())
        {
            splitter.Content(d["content"].get<std::string>(), emit);
        }

        if (d.contains("tool_calls") && d["tool_calls"].is_array())
        {
            for (const auto& tc : d["tool_calls"])
            {
                int index = tc.contains("index") && tc["index"].is_number_integer() ? tc["index"].get<int>()
                                                                                    : static_cast<int>(calls.size());
                json& call = calls[index];
                if (call.is_null())
                {
                    call = { { "id", "" }, { "type", "function" }, { "function", { { "name", "" }, { "arguments", "" } } } };
                }

                if (tc.contains("id") && tc["id"].is_string() && !tc["id"].get<std::string>().empty())
                {
                    call["id"] = tc["id"];
                }

                // Gemini 3: the call's thought signature must come back with it on the next request.
                if (tc.contains("extra_content") && tc["extra_content"].is_object())
                {
                    call["extra_content"] = tc["extra_content"];
                }

                if (tc.contains("function") && tc["function"].is_object())
                {
                    const json& f = tc["function"];
                    if (f.contains("name") && f["name"].is_string())
                    {
                        call["function"]["name"] = call["function"]["name"].get<std::string>() + f["name"].get<std::string>();
                    }

                    if (f.contains("arguments"))
                    {
                        std::string piece = f["arguments"].is_string() ? f["arguments"].get<std::string>() : f["arguments"].dump();
                        call["function"]["arguments"] = call["function"]["arguments"].get<std::string>() + piece;
                    }
                }
            }
        }

        return true;
    };

    HttpHeaders headers = ep.headers;
    headers.push_back({ "Content-Type", "application/json" });
    headers.push_back({ "Accept", "text/event-stream" });
    // Fields starting with "x_" are this application's own (display, Claude's content blocks): not for the server.
    json body = request;
    if (body.contains("messages"))
    {
        for (auto& m : body["messages"])
        {
            for (auto it = m.begin(); it != m.end();)
            {
                it = it.key().rfind("x_", 0) == 0 ? m.erase(it) : std::next(it);
            }
        }
    }

    HttpResult r = HttpSend(L"POST", ep.baseUrl + "/chat/completions", body.dump(-1, ' ', false, json::error_handler_t::replace), headers, 30 * 60 * 1000, [&](const char* data, size_t size)
        { return sse.Feed(data, size, onEvent); }, cancel);

    if (r.Ok() && !done && res.error.empty())
    {
        sse.Finish(onEvent);
    }

    splitter.Flush(emit);

    if (r.cancelled && res.error.empty() && !done)
    {
        res.cancelled = cancel && cancel->IsCancelled();
    }

    if (!r.Ok() && !res.cancelled)
    {
        res.error = ErrorText(r);
    }
    else if (res.error.empty() && !r.error.empty() && !done && !res.cancelled)
    {
        res.error = r.error;
    }

    int n = 0;
    for (auto& [index, call] : calls)
    {
        if (call["id"].get<std::string>().empty())
        {
            call["id"] = "call_" + std::to_string(NowEpoch()) + "_" + std::to_string(n);
        }

        ++n;
        res.toolCalls.push_back(call);
    }

    return res;
}

}  // namespace chat
