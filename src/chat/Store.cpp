#include "Store.h"

#include "Util.h"

#include <algorithm>

namespace chat
{

static std::wstring SettingsPath()
{
    return ChatHome() + L"\\settings.json";
}

static std::wstring ConversationPath(const std::string& id)
{
    return ChatHome() + L"\\conversations\\" + Wide(id) + L".json";
}

bool ProviderConfig::Cloud() const
{
    std::string u = Lower(baseUrl);
    return u.find("://127.0.0.1") == std::string::npos && u.find("://localhost") == std::string::npos &&
           u.find("://[::1]") == std::string::npos;
}

std::string ProviderConfig::Key() const
{
    if (!apiKey.empty())
    {
        return apiKey;
    }

    if (apiKeyEnv.empty())
    {
        return {};
    }

    if (apiKeyEnv == "gh")
    {
        return GhToken();  // the GitHub CLI's sign-in to github.com
    }

    if (apiKeyEnv.rfind("gh:", 0) == 0)
    {
        return GhToken(apiKeyEnv.substr(3));  // ... or to a GitHub Enterprise host
    }

    if (apiKeyEnv == "ant")
    {
        return AntToken();  // Anthropic's CLI sign-in (an OAuth token for the Claude API)
    }

    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(Wide(apiKeyEnv).c_str(), buf, 4096);
    return n > 0 && n < 4096 ? Utf8(std::wstring(buf, n)) : std::string();
}

const ProviderConfig* Settings::FindProvider(const std::string& id) const
{
    for (const auto& p : providers)
    {
        if (p.id == id)
        {
            return &p;
        }
    }

    return nullptr;
}

void Settings::Load()
{
    std::string text;
    if (!ReadFileBytes(SettingsPath(), text))
    {
        return;
    }

    json j = json::parse(text, nullptr, false);
    if (!j.is_object())
    {
        return;
    }

    backend = j.value("backend", backend);
    customUrl = j.value("custom_url", customUrl);
    customKey = j.value("custom_key", customKey);
    model = j.value("model", model);
    reasoning = j.value("reasoning", reasoning);
    defaultSystemMessage = j.value("default_system_message", defaultSystemMessage);
    defaultWorkspace = j.value("default_workspace", defaultWorkspace);
    shell = j.value("shell", shell);
    temperature = j.value("temperature", temperature);
    confirmShell = j.value("confirm_shell", confirmShell);
    confirmWrites = j.value("confirm_writes", confirmWrites);
    showReasoning = j.value("show_reasoning", showReasoning);
    autoStartHub = j.value("auto_start_hub", autoStartHub);
    maxToolRounds = j.value("max_tool_rounds", maxToolRounds);
    providers.clear();
    if (j.contains("providers") && j["providers"].is_array())
    {
        for (const auto& x : j["providers"])
        {
            if (!x.is_object())
            {
                continue;
            }

            ProviderConfig p;
            p.id = x.value("id", "");
            p.name = x.value("name", p.id);
            p.kind = x.value("kind", "openai");
            p.baseUrl = x.value("base_url", "");
            p.apiKey = Unprotect(x.value("api_key_protected", ""));
            p.apiKeyEnv = x.value("api_key_env", "");
            p.enabled = x.value("enabled", true);
            p.inAuto = x.value("in_auto", false);
            if (x.contains("models") && x["models"].is_array())
            {
                for (const auto& m : x["models"])
                {
                    if (m.is_string())
                    {
                        p.models.push_back(m.get<std::string>());
                    }
                }
            }

            // GitHub Models (models.github.ai) was retired on 30 July 2026: its providers can never answer again.
            if (Lower(p.baseUrl).find("models.github.ai") != std::string::npos)
            {
                continue;
            }

            if (!p.id.empty())
            {
                providers.push_back(p);
            }
        }
    }

    autoImportProviders = j.value("auto_import_providers", autoImportProviders);
    ollamaMoveDeclined = j.value("ollama_move_declined", ollamaMoveDeclined);
    if (j.contains("ui") && j["ui"].is_object())
    {
        const json& ui = j["ui"];
        sidebarWidth = std::clamp(ui.value("sidebar_width", sidebarWidth), 200, 480);
        infoWidth = std::clamp(ui.value("info_width", infoWidth), 220, 520);
        sidebarVisible = ui.value("sidebar_visible", sidebarVisible);
        infoVisible = ui.value("info_visible", infoVisible);
    }

    claudeSetupDeclined = j.value("claude_setup_declined", claudeSetupDeclined);
    importedProviders.clear();
    if (j.contains("imported_providers") && j["imported_providers"].is_array())
    {
        for (const auto& s : j["imported_providers"])
        {
            if (s.is_string())
            {
                importedProviders.insert(s.get<std::string>());
            }
        }
    }

    if (j.contains("auto") && j["auto"].is_object())
    {
        const json& a = j["auto"];
        autoRouterModel = a.value("router_model", autoRouterModel);
        autoSticky = a.value("sticky", autoSticky);
        autoSuggestDownloads = a.value("suggest_downloads", autoSuggestDownloads);
        autoOverrides.clear();
        if (a.contains("models") && a["models"].is_object())
        {
            for (auto it = a["models"].begin(); it != a["models"].end(); ++it)
            {
                if (it.value().is_string() && !it.value().get<std::string>().empty())
                {
                    autoOverrides[it.key()] = it.value().get<std::string>();
                }
            }
        }
    }

    disabledTools.clear();
    if (j.contains("disabled_tools") && j["disabled_tools"].is_array())
    {
        for (const auto& t : j["disabled_tools"])
        {
            if (t.is_string())
            {
                disabledTools.insert(t.get<std::string>());
            }
        }
    }

    mcpServers.clear();
    if (j.contains("mcp_servers") && j["mcp_servers"].is_array())
    {
        for (const auto& s : j["mcp_servers"])
        {
            if (!s.is_object())
            {
                continue;
            }

            McpServerConfig c;
            c.name = s.value("name", "");
            c.transport = s.value("transport", "stdio");
            c.command = s.value("command", "");
            c.url = s.value("url", "");
            c.token = s.value("token", "");
            c.enabled = s.value("enabled", true);
            if (!c.name.empty())
            {
                mcpServers.push_back(c);
            }
        }
    }
}

bool Settings::Save() const
{
    json servers = json::array();
    for (const auto& s : mcpServers)
    {
        servers.push_back({ { "name", s.name }, { "transport", s.transport }, { "command", s.command }, { "url", s.url },
            { "token", s.token }, { "enabled", s.enabled } });
    }

    json provs = json::array();
    for (const auto& p : providers)
    {
        provs.push_back({ { "id", p.id }, { "name", p.name }, { "kind", p.kind }, { "base_url", p.baseUrl },
            { "api_key_protected", Protect(p.apiKey) }, { "api_key_env", p.apiKeyEnv }, { "enabled", p.enabled },
            { "in_auto", p.inAuto }, { "models", p.models } });
    }

    json j = { { "backend", backend }, { "providers", provs }, { "auto_import_providers", autoImportProviders },
        { "imported_providers", importedProviders }, { "ollama_move_declined", ollamaMoveDeclined },
        { "ui", { { "sidebar_width", sidebarWidth }, { "info_width", infoWidth }, { "sidebar_visible", sidebarVisible },
                    { "info_visible", infoVisible } } },
        { "claude_setup_declined", claudeSetupDeclined }, { "custom_url", customUrl }, { "custom_key", customKey }, { "model", model },
        { "reasoning", reasoning }, { "default_system_message", defaultSystemMessage },
        { "default_workspace", defaultWorkspace }, { "shell", shell }, { "temperature", temperature },
        { "confirm_shell", confirmShell }, { "confirm_writes", confirmWrites }, { "show_reasoning", showReasoning }, { "auto_start_hub", autoStartHub },
        { "max_tool_rounds", maxToolRounds }, { "disabled_tools", disabledTools }, { "mcp_servers", servers },
        { "auto",
            { { "router_model", autoRouterModel }, { "sticky", autoSticky }, { "suggest_downloads", autoSuggestDownloads },
                { "models", autoOverrides } } } };

    return WriteFileAtomic(SettingsPath(), j.dump(2));
}

bool Conversation::Save() const
{
    json j = { { "id", id }, { "title", title }, { "workspace", workspace }, { "system_message", systemMessage },
        { "created", created }, { "updated", updated }, { "messages", messages } };
    return WriteFileAtomic(ConversationPath(id), j.dump(1, ' ', false, json::error_handler_t::replace));
}

void Conversation::Delete() const
{
    DeleteFileW(ConversationPath(id).c_str());
}

std::vector<std::shared_ptr<Conversation>> LoadConversations()
{
    std::vector<std::shared_ptr<Conversation>> out;
    std::wstring dir = ChatHome() + L"\\conversations\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return out;
    }

    do
    {
        std::string text;
        if (!ReadFileBytes(dir + fd.cFileName, text))
        {
            continue;
        }

        json j = json::parse(text, nullptr, false);
        if (!j.is_object())
        {
            continue;
        }

        auto c = std::make_shared<Conversation>();
        c->id = j.value("id", "");
        c->title = j.value("title", "");
        c->workspace = j.value("workspace", "");
        c->systemMessage = j.value("system_message", "");
        c->created = j.value("created", 0LL);
        c->updated = j.value("updated", 0LL);
        if (j.contains("messages") && j["messages"].is_array())
        {
            c->messages = j["messages"];
        }

        if (!c->id.empty())
        {
            out.push_back(c);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b)
        { return a->updated > b->updated; });
    return out;
}

}  // namespace chat
