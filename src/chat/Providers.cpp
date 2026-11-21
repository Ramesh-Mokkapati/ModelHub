#include "Providers.h"

#include "Codex.h"
#include "Copilot.h"
#include "Util.h"

#include <algorithm>

namespace chat
{

std::string QualifiedModel(const std::string& providerId, const std::string& model)
{
    return providerId + kProviderSep + model;
}

bool SplitQualified(const std::string& qualified, std::string& providerId, std::string& model)
{
    size_t bar = qualified.find(kProviderSep);
    if (bar == std::string::npos)
    {
        return false;
    }

    providerId = qualified.substr(0, bar);
    model = qualified.substr(bar + 1);
    return true;
}

std::string DisplayModel(const Settings& s, const std::string& qualified)
{
    std::string id, model;
    if (!SplitQualified(qualified, id, model))
    {
        return qualified;
    }

    const ProviderConfig* p = s.FindProvider(id);
    return model + "  (" + (p ? p->name : id) + ")";
}

const std::vector<ProviderPreset>& ProviderPresets()
{
    static const std::vector<ProviderPreset> presets = {
        { "Claude (Anthropic API)", "claude", "Claude", "anthropic", "https://api.anthropic.com/v1", "ANTHROPIC_API_KEY",
            "https://console.anthropic.com/settings/keys" },
        { "ChatGPT (your ChatGPT sign-in, through the Codex app or CLI)", "chatgpt", "ChatGPT", "codex",
            "https://chatgpt.com", "", "https://chatgpt.com/codex" },
        { "ChatGPT models (OpenAI API)", "openai", "OpenAI", "openai", "https://api.openai.com/v1", "OPENAI_API_KEY",
            "https://platform.openai.com/api-keys" },
        { "Gemini (Google AI, OpenAI-compatible)", "gemini", "Gemini", "openai",
            "https://generativelanguage.googleapis.com/v1beta/openai", "GEMINI_API_KEY", "https://aistudio.google.com/apikey" },
        { "GitHub Copilot (your Copilot subscription, github.com)", "copilot", "GitHub Copilot", "copilot",
            "https://github.com", "", "https://github.com/features/copilot" },
        { "GitHub Copilot on GitHub Enterprise Cloud (edit <host>)", "copilot-ghe", "GitHub Copilot (Enterprise)", "copilot",
            "https://<host>.ghe.com", "", "https://github.com/features/copilot" },
        { "Mistral", "mistral", "Mistral", "openai", "https://api.mistral.ai/v1", "MISTRAL_API_KEY",
            "https://console.mistral.ai/api-keys" },
        { "OpenRouter", "openrouter", "OpenRouter", "openai", "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY",
            "https://openrouter.ai/keys" },
        { "Groq", "groq", "Groq", "openai", "https://api.groq.com/openai/v1", "GROQ_API_KEY", "https://console.groq.com/keys" },
        { "DeepSeek", "deepseek", "DeepSeek", "openai", "https://api.deepseek.com/v1", "DEEPSEEK_API_KEY",
            "https://platform.deepseek.com/api_keys" },
        { "Ollama (on this PC)", "ollama", "Ollama", "openai", "http://127.0.0.1:11434/v1", "", "" },
        { "LM Studio (on this PC)", "lmstudio", "LM Studio", "openai", "http://127.0.0.1:1234/v1", "", "" },
        { "Jan (on this PC)", "jan", "Jan", "openai", "http://127.0.0.1:1337/v1", "", "" },
        { "llama.cpp server (on this PC)", "llamacpp", "llama-server", "openai", "http://127.0.0.1:8080/v1", "", "" },
        { "Other OpenAI-compatible server", "custom", "Custom", "openai", "http://127.0.0.1:8000/v1", "", "" },
    };

    return presets;
}

static ProviderConfig FromPreset(const ProviderPreset& p)
{
    ProviderConfig c;
    c.id = p.id;
    c.name = p.name;
    c.kind = p.kind;
    c.baseUrl = p.baseUrl;
    c.apiKeyEnv = p.keyEnv;
    return c;
}

static std::wstring EnvW(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n > 0 && n < MAX_PATH * 2 ? std::wstring(buf, n) : std::wstring();
}

static bool Exists(const std::wstring& path)
{
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ProviderEndpoint(const ProviderConfig& p, Endpoint& ep, std::string& error)
{
    ep = Endpoint{};
    ep.baseUrl = TrimCopy(p.baseUrl);
    while (!ep.baseUrl.empty() && ep.baseUrl.back() == '/')
    {
        ep.baseUrl.pop_back();
    }

    if (ep.baseUrl.empty())
    {
        error = p.name + ": no URL";
        return false;
    }

    std::string key = p.Key();
    ep.kind = p.kind;
    ep.label = p.name;
    if (p.kind == "codex")
    {
        return true;  // the Codex CLI signs in by itself (codex login)
    }

    if (p.kind == "copilot")
    {
        return true;  // the Copilot runtime signs in by itself (copilot login); baseUrl names the GitHub host
    }

    if (p.kind == "anthropic")
    {
        if (key.empty())
        {
            error = p.apiKeyEnv == "ant" ? p.name + ": not signed in (run: ant auth login)"
                                         : p.name + ": no API key (Model providers: add one, or set " +
                                               (p.apiKeyEnv.empty() ? std::string("ANTHROPIC_API_KEY") : p.apiKeyEnv) + ")";
            return false;
        }

        if (p.apiKeyEnv == "ant")
        {
            // An OAuth token from Anthropic's CLI goes on Authorization, with the OAuth beta.
            ep.headers.push_back({ "Authorization", "Bearer " + key });
            ep.headers.push_back({ "anthropic-beta", "oauth-2025-04-20" });
        }
        else
        {
            ep.headers.push_back({ "x-api-key", key });
        }

        ep.headers.push_back({ "anthropic-version", "2026-10-21" });
    }
    else if (!key.empty())
    {
        ep.headers.push_back({ "Authorization", "Bearer " + key });
    }
    else if (p.Cloud())
    {
        error = p.name + (p.apiKeyEnv.rfind("gh", 0) == 0
                                 ? ": not signed in (run: gh auth login" +
                                       (p.apiKeyEnv.size() > 3 ? " --hostname " + p.apiKeyEnv.substr(3) : std::string()) +
                                       ", or add a GitHub token with models:read)"
                                 : ": no API key" + (p.apiKeyEnv.empty() ? std::string() : " (or set " + p.apiKeyEnv + ")"));
        return false;
    }

    return true;
}

bool ListProviderModels(const ProviderConfig& p, std::vector<std::string>& models, std::string& error)
{
    models.clear();
    Endpoint ep;
    if (!ProviderEndpoint(p, ep, error))
    {
        return false;
    }

    if (p.kind == "copilot")
    {
        return CopilotListModels(ep.baseUrl, models, error);
    }

    if (p.kind == "codex")
    {
        return CodexListModels(models, error);
    }

    std::string url = ep.baseUrl + "/models" + (p.kind == "anthropic" ? "?limit=1000" : "");
    HttpResult r = HttpSend(L"GET", url, "", ep.headers, p.Cloud() ? 20000 : 4000);
    if (!r.Ok())
    {
        error = r.status == 0 ? r.error
                : r.status == 401 || r.status == 403
                    ? "the API key was not accepted (HTTP " + std::to_string(r.status) + ")"
                    : "HTTP " + std::to_string(r.status) + ": " + Truncate(r.body, 200);
        return false;
    }

    json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded())
    {
        error = "the answer was not JSON (\"" + Truncate(TrimCopy(r.body), 60, "...") +
                "\"): a proxy or security software on this network may be answering in its place";
        return false;
    }

    const json* list = j.is_array()                                                      ? &j
                       : j.is_object() && j.contains("data") && j["data"].is_array()     ? &j["data"]
                       : j.is_object() && j.contains("models") && j["models"].is_array() ? &j["models"]
                                                                                         : nullptr;
    if (list)
    {
        for (const auto& m : *list)
        {
            std::string id = m.is_object() ? m.value("id", m.value("name", "")) : std::string();
            if (id.rfind("models/", 0) == 0)
            {
                id.erase(0, 7);  // Gemini
            }

            std::string low = Lower(id);

            // Chat models only: not embeddings, speech, images or moderation.
            bool other = low.find("embed") != std::string::npos || low.find("tts") != std::string::npos ||
                         low.find("whisper") != std::string::npos || low.find("dall-e") != std::string::npos ||
                         low.find("moderation") != std::string::npos || low.find("transcribe") != std::string::npos ||
                         low.find("image") != std::string::npos || low.find("realtime") != std::string::npos ||
                         low.find("audio") != std::string::npos;

            // Gemini's models that do not chat over this API: music (Lyria), worlds (Genie), video (Veo), images
            // (Imagen, Nano Banana), live audio, robotics, computer use, and attributed QA.
            for (const char* name : { "lyria", "genie", "veo", "imagen", "nano-banana", "-live", "robotics",
                     "computer-use", "live-translate", "aqa" })
            {
                other = other || low.find(name) != std::string::npos;
            }

            if (!id.empty() && !other)
            {
                models.push_back(id);
            }
        }
    }

    std::sort(models.begin(), models.end());
    models.erase(std::unique(models.begin(), models.end()), models.end());
    if (models.empty())
    {
        error = "no chat models listed";
    }

    return !models.empty();
}

std::string ProviderSignature(const ProviderConfig& p)
{
    std::string url = Lower(TrimCopy(p.baseUrl));
    while (!url.empty() && url.back() == '/')
    {
        url.pop_back();
    }

    return p.kind + "|" + url + "|" + p.apiKeyEnv;
}

std::string UniqueProviderId(const std::vector<ProviderConfig>& list, std::string base)
{
    for (char& c : base)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
        {
            c = '-';
        }
    }

    base = Lower(base.empty() ? std::string("provider") : base);
    std::string id = base;
    for (int n = 2;; ++n)
    {
        bool taken = std::any_of(list.begin(), list.end(), [&](const ProviderConfig& p)
            { return p.id == id; });
        if (!taken)
        {
            return id;
        }

        id = base + std::to_string(n);
    }
}

namespace
{

const ProviderPreset* Preset(const char* id)
{
    for (const auto& p : ProviderPresets())
    {
        if (std::string(p.id) == id)
        {
            return &p;
        }
    }

    return nullptr;
}

// "0.0.0.0", "0.0.0.0:11435", "http://127.0.0.1:11435" -> the port, or 0.
int PortOf(std::string hostPort)
{
    size_t scheme = hostPort.find("://");
    if (scheme != std::string::npos)
    {
        hostPort.erase(0, scheme + 3);
    }

    size_t colon = hostPort.rfind(':');
    return colon == std::string::npos ? 0 : std::atoi(hostPort.c_str() + colon + 1);
}

std::string WithPort(const std::string& url, int port)
{
    if (port <= 0)
    {
        return url;
    }

    size_t hostStart = url.find("://") + 3, colon = url.find(':', hostStart), slash = url.find('/', hostStart);
    return url.substr(0, colon) + ":" + std::to_string(port) + url.substr(slash);
}

// A server on this PC: running (with its models), installed, or neither (nothing to add).
void LocalServer(Detection& d, const char* presetId, const std::vector<std::wstring>& installPaths, int port)
{
    const ProviderPreset* preset = Preset(presetId);
    if (!preset)
    {
        return;
    }

    ProviderConfig c = FromPreset(*preset);
    c.baseUrl = WithPort(c.baseUrl, port);
    c.inAuto = true;  // on this PC: nothing leaves it
    std::vector<std::string> models;
    std::string error;
    if (ListProviderModels(c, models, error))
    {
        c.models = models;
        d.found.push_back({ c, std::string(preset->name) + " is running with " + std::to_string(models.size()) +
                                   (models.size() == 1 ? " model" : " models") });
    }
    else if (std::any_of(installPaths.begin(), installPaths.end(), [](const std::wstring& p)
                 { return Exists(p); }))
    {
        d.found.push_back({ c, std::string(preset->name) + " is installed: start it (and its local server) to use its models" });
    }
}

}  // namespace

Detection DetectProviders()
{
    Detection d;
    std::wstring local = EnvW(L"LOCALAPPDATA"), home = EnvW(L"USERPROFILE"), roaming = EnvW(L"APPDATA");
    std::wstring programs = EnvW(L"ProgramFiles");

    // API keys in the environment.
    for (const auto& preset : ProviderPresets())
    {
        std::string env = preset.keyEnv;
        if (env.empty() || env.rfind("gh", 0) == 0)
        {
            continue;
        }

        std::wstring name = Wide(env);
        std::wstring value = EnvW(name.c_str());
        if (value.empty() && std::string(preset.id) == "gemini")
        {
            value = EnvW(L"GOOGLE_API_KEY"), name = L"GOOGLE_API_KEY";
        }

        if (value.empty())
        {
            continue;
        }

        DetectedProvider f{ FromPreset(preset), "an API key is in " + Utf8(name) };
        f.config.apiKeyEnv = Utf8(name);
        d.found.push_back(f);
    }

    // Claude through Anthropic's CLI sign-in (ant auth login): an OAuth token for the Claude API.
    if (!AntToken().empty())
    {
        if (const ProviderPreset* preset = Preset("claude"))
        {
            ProviderConfig c = FromPreset(*preset);
            c.apiKeyEnv = "ant";
            d.found.push_back({ c, "signed in with Anthropic's CLI (ant auth login)" });
        }
    }

    // GitHub Copilot: one provider per GitHub host this PC knows an account on (the GitHub CLI's sign-ins, and the
    // Credential Manager entries of the GitHub Copilot app, Git and Visual Studio), through the Copilot runtime the
    // GitHub Copilot app installs. A host whose Copilot sign-in is missing, or that has no Copilot models, is added
    // switched off: Model Providers > Edit > Sign in runs "copilot login --host ...".
    if (!CopilotRuntimeExe().empty())
    {
        std::vector<std::string> hosts = GhHosts();
        for (const auto& h : GitHubHostsFromCredentials())
        {
            if (std::find(hosts.begin(), hosts.end(), h) == hosts.end())
            {
                hosts.push_back(h);
            }
        }

        for (const auto& host : hosts)
        {
            bool dotcom = host == "github.com";
            const ProviderPreset* preset = Preset(dotcom ? "copilot" : "copilot-ghe");
            if (!preset)
            {
                continue;
            }

            ProviderConfig c = FromPreset(*preset);
            c.baseUrl = "https://" + host;
            if (!dotcom)
            {
                c.id = "copilot-" + host.substr(0, host.find('.'));
                c.name = "GitHub Copilot (" + host + ")";
            }

            std::vector<std::string> models;
            std::string error;
            c.enabled = CopilotListModels(c.baseUrl, models, error);
            c.models = models;
            d.found.push_back({ c, c.enabled ? "Copilot on " + host + " with " + std::to_string(models.size()) + " models"
                                             : "Copilot on " + host + ": " + error });
        }
    }
    else if (Exists(local + L"\\Programs\\GitHub Copilot"))
    {
        d.notes.push_back("The GitHub Copilot app is installed, but its Copilot runtime was not found (it is downloaded "
                          "the first time the app runs a session). Open the GitHub Copilot app once, then Detect again.");
    }

    // ChatGPT: the Codex app's (or npm's) Codex CLI, with the ChatGPT sign-in it keeps.
    bool codex = !CodexExe().empty();
    if (codex)
    {
        if (const ProviderPreset* preset = Preset("chatgpt"))
        {
            ProviderConfig c = FromPreset(*preset);
            std::vector<std::string> models;
            std::string error, how;
            bool signedIn = CodexSignedIn(&how);
            c.enabled = CodexListModels(models, error);
            c.models = models;
            d.found.push_back({ c, c.enabled ? "Codex " + how + ", with " + std::to_string(models.size()) + " models"
                                             : "Codex: " + error });
            (void)signedIn;
        }
    }

    // Model servers on this PC: their configured ports, running or installed.
    int ollamaPort = PortOf(Utf8(EnvW(L"OLLAMA_HOST")));
    LocalServer(d, "ollama",
        { local + L"\\Programs\\Ollama\\ollama.exe", programs + L"\\Ollama\\ollama.exe" }, ollamaPort);
    int lmPort = 0;
    std::string lmConfig;
    if (ReadFileBytes(home + L"\\.lmstudio\\.internal\\http-server-config.json", lmConfig))
    {
        json j = json::parse(lmConfig, nullptr, false);
        if (j.is_object())
        {
            lmPort = j.value("port", 0);
        }
    }

    LocalServer(d, "lmstudio",
        { programs + L"\\LM Studio\\LM Studio.exe", local + L"\\Programs\\LM Studio\\LM Studio.exe",
            local + L"\\Programs\\lm-studio\\LM Studio.exe", home + L"\\.lmstudio\\bin\\lms.exe" },
        lmPort);

    LocalServer(d, "jan",
        { local + L"\\Programs\\jan\\Jan.exe", local + L"\\Programs\\Jan\\Jan.exe", programs + L"\\Jan\\Jan.exe" }, 0);

    LocalServer(d, "llamacpp", {}, 0);
    // Models Ollama downloaded: ModelHub can take them over (ModelHub Chat offers it).
    d.ollama = FindOllamaModels();
    if (!d.ollama.models.empty())
    {
        d.notes.push_back("Ollama's models are on this PC (" + std::to_string(d.ollama.models.size()) + ", " +
                          std::to_string(d.ollama.totalBytes >> 30) + " GB). The \"+\" menu's \"Move Ollama models to "
                                                                      "ModelHub\" moves them into the shared ModelHub store.");
    }

    // Subscription apps: they do not lend their models to other programs.
    bool claudeApp = Exists(local + L"\\AnthropicClaude") || Exists(local + L"\\Programs\\Claude") ||
                     Exists(local + L"\\Microsoft\\WindowsApps\\claude.exe");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((local + L"\\Packages\\Claude_*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        claudeApp = true;
        FindClose(h);
    }

    bool claudeViaAnt = std::any_of(d.found.begin(), d.found.end(), [](const DetectedProvider& f)
        { return f.config.kind == "anthropic"; });
    d.claudeApp = claudeApp || Exists(home + L"\\.claude");
    if (d.claudeApp && !claudeViaAnt)
    {
        d.notes.push_back("Claude (the app or Claude Code) is installed. Its Pro or Max subscription works only in "
                          "Anthropic's own apps. To use Claude models here, the \"+\" menu's \"Set up Claude\" installs "
                          "Anthropic's CLI and signs in to your Anthropic Console account (or add an API key).");
    }

    // Antigravity: its Google sign-in works only inside Antigravity (Google does not let other programs use it). Its
    // Gemini models are open to other programs through a Gemini API key.
    bool geminiKey = std::any_of(d.found.begin(), d.found.end(), [](const DetectedProvider& f)
        { return f.config.baseUrl.find("generativelanguage.googleapis.com") != std::string::npos; });
    if (!geminiKey && (Exists(local + L"\\Programs\\Antigravity\\Antigravity.exe") || Exists(home + L"\\.gemini\\antigravity")))
    {
        d.notes.push_back("Google Antigravity is installed. Its Google sign-in works only inside Antigravity. To use the "
                          "same Gemini models here, add the \"Gemini\" provider with a free API key from "
                          "aistudio.google.com/apikey (or set GEMINI_API_KEY), then Detect again.");
    }

    if (!codex && (Exists(local + L"\\Microsoft\\WindowsApps\\ChatGPT.exe") || Exists(local + L"\\Programs\\ChatGPT")))
    {
        d.notes.push_back("The ChatGPT desktop app is installed. Its subscription works only inside that app; to use "
                          "OpenAI models here, add an API key from platform.openai.com (OpenAI provider).");
    }

    return d;
}

std::vector<std::string> ImportDetected(Settings& s, const Detection& d)
{
    std::vector<std::string> added;
    for (const auto& p : s.providers)
    {
        s.importedProviders.insert(ProviderSignature(p));  // set up by hand or earlier: never added twice
    }

    for (const auto& f : d.found)
    {
        std::string sig = ProviderSignature(f.config);
        if (s.importedProviders.count(sig))
        {
            continue;  // already there, or removed by the user
        }

        ProviderConfig c = f.config;
        c.id = UniqueProviderId(s.providers, c.id);
        s.providers.push_back(c);
        s.importedProviders.insert(sig);
        added.push_back(c.name + (c.enabled ? "" : " (sign-in needed)"));
    }
    return added;
}

}  // namespace chat
