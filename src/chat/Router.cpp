#include "Router.h"

#include "Util.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <regex>

namespace chat
{

namespace
{

const char* const kIds[kCategoryCount] = { "quick", "general", "code", "reasoning", "agent", "vision" };
const char* const kLabels[kCategoryCount] = { "Quick questions", "General", "Code", "Reasoning", "Agent (tools)", "Images" };

bool Has(const std::string& s, const char* needle)
{
    return s.find(needle) != std::string::npos;
}

// Keywords match whole words: "api" is not in "capital". A keyword that starts or ends with a symbol or a
// space ("c++", "def ", "```") matches at that end as written.
bool HasWord(const std::string& s, const std::string& k)
{
    auto word = [](char ch)
    { return std::isalnum(static_cast<unsigned char>(ch)) != 0; };
    for (size_t at = s.find(k); at != std::string::npos; at = s.find(k, at + 1))
    {
        bool left = !word(k.front()) || at == 0 || !word(s[at - 1]);
        size_t end = at + k.size();
        bool right = !word(k.back()) || end >= s.size() || !word(s[end]) ||
                     (s[end] == 's' && (end + 1 >= s.size() || !word(s[end + 1])));  // plurals: "bugs"
        if (left && right)
        {
            return true;
        }
    }

    return false;
}

int CountAny(const std::string& s, std::initializer_list<const char*> needles)
{
    int n = 0;
    for (const char* k : needles)
    {
        if (HasWord(s, k))
        {
            ++n;
        }
    }

    return n;
}

bool EndsWithAny(const std::string& name, std::initializer_list<const char*> exts)
{
    std::string n = Lower(name);
    for (const char* e : exts)
    {
        size_t len = std::strlen(e);
        if (n.size() >= len && n.compare(n.size() - len, len, e) == 0)
        {
            return true;
        }
    }

    return false;
}

// "qwen3-coder-30b-a3b" -> 30.5 / 3.3 style numbers when nothing better is known.
void ParamsFromName(const std::string& id, double& params, double& active)
{
    std::smatch m;
    std::string s = Lower(id);
    static const std::regex total(R"((\d+(?:\.\d+)?)b(?![a-z]))"), act(R"(a(\d+(?:\.\d+)?)b)");
    params = active = 0;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), total); it != std::sregex_iterator(); ++it)
    {
        size_t pos = static_cast<size_t>(it->position(0));
        if (pos > 0 && s[pos - 1] == 'a')
        {
            continue;  // the "a3b" of a mixture-of-experts name
        }

        params = std::max(params, std::stod((*it)[1].str()));
    }
    if (std::regex_search(s, m, act))
    {
        active = std::stod(m[1].str());
    }

    if (active <= 0)
    {
        active = params;
    }
}

bool IsCoder(const std::string& id)
{
    std::string s = Lower(id);
    return Has(s, "coder") || Has(s, "devstral") || Has(s, "codestral") || Has(s, "starcoder") || Has(s, "codellama") ||
           Has(s, "codegemma") || Has(s, "codex") || Has(s, "mai-code");
}

bool IsReasoner(const std::string& id)
{
    std::string s = Lower(id);
    return (Has(s, "qwen3") && !Has(s, "coder")) || Has(s, "gpt-oss") || Has(s, "phi-4") || Has(s, "-r1") ||
           Has(s, "qwq") || Has(s, "magistral") || Has(s, "thinking");
}

bool IsVision(const std::string& id)
{
    std::string s = Lower(id);
    return Has(s, "-vl") || Has(s, "vision") || Has(s, "llava") || Has(s, "pixtral") || Has(s, "minicpm-v") ||
           Has(s, "gemma-3") || Has(s, "moondream");
}

std::string Root(const Endpoint& ep)
{
    return ep.baseUrl.size() > 3 && ep.baseUrl.compare(ep.baseUrl.size() - 3, 3, "/v1") == 0
               ? ep.baseUrl.substr(0, ep.baseUrl.size() - 3)
               : ep.baseUrl;
}

json GetJson(const Endpoint& ep, const std::string& url, const std::string& body = {})
{
    HttpHeaders h = ep.headers;
    if (!body.empty())
    {
        h.push_back({ "Content-Type", "application/json" });
    }

    HttpResult r = HttpSend(body.empty() ? L"GET" : L"POST", url, body, h, 20000);
    json j = json::parse(r.Ok() ? r.body : std::string("null"), nullptr, false);
    return j.is_discarded() ? json() : j;
}

// A cloud model: its size is not published, so its standing comes from its family name.
void CloudProfile(ModelInfo& m, const std::string& name)
{
    std::string s = Lower(name);
    auto any = [&](std::initializer_list<const char*> keys)
    {
        for (const char* k : keys)
        {
            if (Has(s, k))
            {
                return true;
            }
        }

        return false;
    };

    bool isSmall = any({ "haiku", "mini", "flash", "nano", "lite", "small", "8b", "7b" });
    m.quality = any({ "fable", "mythos" })                                                                          ? 75
                : any({ "opus" })                                                                                   ? 70
                : any({ "gpt-6" }) && !isSmall                                                                      ? 72
                : any({ "gpt-5", "o3", "o4", "gemini-2.5-pro", "gemini-3", "grok-4", "mai-code" }) && !isSmall      ? 68
                : any({ "sonnet", "gpt-4.1", "gpt-4o", "-pro", "large", "deepseek-r1", "deepseek-v3" }) && !isSmall ? 63
                : isSmall                                                                                           ? 50
                                                                                                                    : 56;
    m.speed = isSmall ? "fast" : any({ "opus", "fable", "mythos", "pro", "o3" }) ? "slow"
                                                                                 : "moderate";
    m.tools = true;
    m.vision = Has(s, "claude") || Has(s, "gpt-4") || Has(s, "gpt-5") || Has(s, "gemini") || Has(s, "pixtral") || IsVision(s);
    m.contextK = Has(s, "claude") ? 1000 : 128;
}

std::vector<ModelInfo> LoadInfo(const Endpoint& ep, const std::vector<std::string>& ids, int contextTokens,
    const std::set<std::string>& cloudProviders)
{
    std::map<std::string, ModelInfo> byId;
    for (const auto& id : ids)
    {
        ModelInfo m;
        m.id = id;
        size_t bar = id.find('|');
        std::string name = bar == std::string::npos ? id : id.substr(bar + 1);
        ParamsFromName(name, m.paramsB, m.activeParamsB);
        m.tier = m.paramsB >= 14 ? 3 : m.paramsB >= 7 ? 2
                                                      : 1;
        m.vision = IsVision(name);
        if (bar != std::string::npos)
        {
            m.provider = true;
            m.cloud = cloudProviders.count(id.substr(0, bar)) > 0;
            if (m.cloud)
            {
                CloudProfile(m, name);
                m.fit = "cloud";
            }
        }

        byId[id] = m;
    }

    if (ep.modelhub)
    {
        std::string root = Root(ep);
        json models = GetJson(ep, root + "/hub/v1/catalog");
        if (!models.is_object() || !models.contains("models"))
        {
            models = GetJson(ep, root + "/hub/v1/models");
        }

        if (models.is_object() && models.contains("models"))
        {
            for (const auto& j : models["models"])
            {
                std::string id = j.value("id", "");
                auto it = byId.find(id);
                if (it == byId.end())
                {
                    // Not installed: a candidate to suggest (chat models only).
                    if (j.value("installed", false) || j.value("embeddings", false) || id.empty())
                    {
                        continue;
                    }

                    ModelInfo m;
                    m.id = id;
                    m.installed = false;
                    m.vision = IsVision(id);
                    it = byId.emplace(id, m).first;
                }

                ModelInfo& m = it->second;
                m.sizeBytes = j.value("size", 0ULL);
                m.tier = j.value("tier", m.tier);
                m.paramsB = j.value("params_b", m.paramsB);
                m.activeParamsB = j.value("active_params_b", m.paramsB);
                m.tools = j.value("tools", true);
                m.contextK = j.value("context_k", 32);
            }
        }

        json fits = GetJson(ep, root + "/hub/v1/fits",
            json{ { "context_tokens", std::max(8192, contextTokens) }, { "parallel", 1 } }.dump());
        if (fits.is_object() && fits.contains("models"))
        {
            for (const auto& j : fits["models"])
            {
                auto it = byId.find(j.value("id", ""));
                if (it != byId.end())
                {
                    it->second.fit = j.value("fit", "");
                    it->second.speed = j.value("speed", "");
                    it->second.fitsDisk = j.value("fits_disk", true);
                }
            }
        }

        json status = GetJson(ep, root + "/hub/v1/status");
        if (status.is_object() && status.contains("servers"))
        {
            for (const auto& s : status["servers"])
            {
                auto it = byId.find(s.value("model", ""));
                if (it != byId.end() && s.value("context_tokens", 0) >= contextTokens)
                {
                    it->second.running = true;
                }
            }
        }
    }

    std::vector<ModelInfo> out;
    for (auto& [id, m] : byId)
    {
        out.push_back(m);
    }

    return out;
}

// Higher is better; NAN: not usable for this request.
double Score(const ModelInfo& m, Category c, int contextTokens, std::string* why)
{
    if (!m.cloud && m.paramsB > 0 && m.paramsB < 1.0)
    {
        return NAN;  // tiny models are for checking a setup, not for answers
    }

    if (m.fit == "too-large")
    {
        return NAN;
    }

    if (m.contextK > 0 && contextTokens > m.contextK * 1024)
    {
        return NAN;
    }

    if (c == Category::Agent && !m.tools)
    {
        return NAN;
    }

    if (c == Category::Vision && !m.vision)
    {
        return NAN;
    }

    double quality = m.quality > 0 ? m.quality : m.tier * 10.0 + std::min(m.paramsB, 32.0) / 3.0;
    std::string low = Lower(m.id);
    bool coder = IsCoder(m.id),
         reasoner = IsReasoner(m.id) || (m.cloud && (Has(low, "claude") || Has(low, "gpt-5") || Has(low, "o3") ||
                                                        Has(low, "o4") || Has(low, "gemini-2.5") || Has(low, "gemini-3")));
    switch (c)
    {
    case Category::Code:
        quality += coder ? 12 : -4;
        break;
    case Category::Agent:
        quality += coder ? 8 : 0;
        if (Has(Lower(m.id), "devstral") || Has(Lower(m.id), "qwen3-coder"))
        {
            quality += 6;  // trained for agentic tool use
        }
        break;
    case Category::Reasoning:
        quality += reasoner ? 12 : coder ? -3
                                         : 0;
        break;
    case Category::General:
    case Category::Quick:
    case Category::Vision:
        quality += coder ? -4 : 8;
        break;
    }

    double speed;
    if (!m.speed.empty())
    {
        speed = m.speed == "fast" ? 12 : m.speed == "moderate" ? 4
                                                               : -12;
    }
    else
    {
        speed = m.activeParamsB <= 4 ? 12 : m.activeParamsB <= 9 ? 4
                                                                 : -8;
    }

    if (m.fit == "gpu")
    {
        speed += 6;
    }
    else if (m.fit == "cpu")
    {
        speed -= 2;
    }
    else if (m.fit == "cpu-tight")
    {
        speed -= 25;
    }
    else if (m.fit == "cloud")
    {
        speed -= 2;  // a network round trip, but no loading
    }

    if (m.running)
    {
        speed += 5;  // loaded already: no wait
    }

    static const double weights[kCategoryCount][2] = {
        { 0.5, 2.0 },  // quick: speed first
        { 1.0, 1.0 },  // general
        { 1.3, 0.8 },  // code
        { 1.5, 0.6 },  // reasoning: quality first
        { 1.2, 1.0 },  // agent
        { 1.0, 1.0 },  // vision
    };

    const double* w = weights[static_cast<int>(c)];
    if (why)
    {
        *why = m.cloud ? std::string("cloud, ") + (m.speed.empty() ? "" : m.speed)
                       : (m.speed.empty() ? std::string() : m.speed + (m.fit.empty() ? "" : " on " + m.fit)) +
                             (m.running ? ", already loaded" : "");
    }

    return w[0] * quality + w[1] * speed;
}

}  // namespace

const char* CategoryId(Category c)
{
    return kIds[static_cast<int>(c)];
}

const char* CategoryLabel(Category c)
{
    return kLabels[static_cast<int>(c)];
}

bool ParseCategory(const std::string& id, Category& out)
{
    for (int i = 0; i < kCategoryCount; ++i)
    {
        if (id == kIds[i])
        {
            out = static_cast<Category>(i);
            return true;
        }
    }

    return false;
}

Classification Classify(const RouteRequest& req)
{
    Classification c;
    c.estimatedTokens = static_cast<int>(req.promptChars / 3.5) + static_cast<int>(req.toolCount) * 150 + 1024;
    std::string t = Lower(req.text);

    if (req.hasImages)
    {
        c.category = Category::Vision;
        c.reason = "an image is attached";
        return c;
    }

    bool codeFiles = false;
    for (const auto& n : req.attachmentNames)
    {
        codeFiles = codeFiles || EndsWithAny(n, { ".c", ".cpp", ".cc", ".h", ".hpp", ".cs", ".java", ".py", ".js", ".ts",
                                                    ".tsx", ".go", ".rs", ".rb", ".php", ".swift", ".kt", ".sql", ".ps1",
                                                    ".sh", ".bat", ".vcxproj", ".cmake", ".json", ".xml", ".yaml", ".yml" });
    }

    int code = CountAny(t, { "```", "function", "class ", "compile", "compiler", "linker", "exception", "stack trace",
                               "refactor", "implement", "code", "script", "python", "c++", "c#", "java", "javascript",
                               "typescript", "sql", "regex", "unit test", "debug", "cmake", "msbuild", "syntax", "#include",
                               "std::", "def ", "public ", "struct ", "variable", "algorithm", "api", "bug", "mfc", "win32" }) +
               (codeFiles ? 3 : 0);

    static const std::regex compilerError(R"(\b(error|warning) (c|lnk)\d{4}\b)");
    if (std::regex_search(t, compilerError))
    {
        code += 3;
    }

    int agent = req.toolCount == 0
                    ? 0
                    : CountAny(t, { "this folder", "the folder", "workspace", "my files", "in the repo", "this repo",
                                      "the project", "this project", "run the", "run it", "execute", "build it", "build the",
                                      "search for", "find the file", "find files", "list the files", "list files", "open the file",
                                      "read the file", "edit the", "modify the", "create a file", "write a file", "save it",
                                      "save the", "fix the file", "grep", "directory", "c:\\", "src/", "src\\", "tests pass",
                                      "use the tool", "use file_", "use grep", "call the tool", "git " });

    int reasoning = CountAny(t, { "why ", "prove", "step by step", "calculate", "solve", "math", "logic", "puzzle",
                                    "analyze", "analyse", "compare", "trade-off", "tradeoff", "pros and cons", "derive",
                                    "equation", "probability", "estimate", "how many", "plan ", "strategy", "design " });
    static const std::regex arithmetic(R"(\d+\s*[\+\-\*/\^x]\s*\d+)");
    if (std::regex_search(t, arithmetic))
    {
        reasoning += 2;
    }

    // A short reply in a running conversation ("yes, do it", "continue") keeps the conversation's kind.
    Category previous;
    if (req.text.size() < 80 && code < 2 && agent == 0 && reasoning == 0 && !req.previousCategory.empty() &&
        ParseCategory(req.previousCategory, previous) && previous != Category::Vision)
    {
        c.category = previous;
        c.followUp = true;
        c.reason = "a follow-up in a " + std::string(CategoryId(previous)) + " conversation";
        return c;
    }

    if (agent >= 1)
    {
        c.category = Category::Agent;
        c.reason = "it asks for work on files or commands (tools)";
    }
    else if (code >= 2 || codeFiles)
    {
        c.category = Category::Code;
        c.reason = codeFiles ? "code files are attached" : "it is about code";
    }
    else if (reasoning >= 2 || (reasoning >= 1 && req.text.size() > 200))
    {
        c.category = Category::Reasoning;
        c.reason = "it needs careful reasoning";
    }
    else if (req.text.size() < 140 && req.attachmentNames.empty() && code == 0)
    {
        c.category = Category::Quick;
        c.reason = "a short question";
    }
    else
    {
        c.category = Category::General;
        c.reason = "a general request";
    }

    return c;
}

RouteResult Route(const Endpoint& ep, const Settings& settings, const RouteRequest& req,
    const std::vector<std::string>& models, CancelToken* cancel, const std::function<void(const std::string&)>& status,
    const std::set<std::string>& cloudProviders)
{
    RouteResult r;
    if (models.empty())
    {
        r.error = "Auto mode: no models to choose from";
        return r;
    }

    Classification c = Classify(req);
    r.category = c.category;
    r.reason = c.reason;
    r.contextTokens = std::max(8192, ((c.estimatedTokens * 3 / 2) / 4096 + 1) * 4096);  // room for the answer

    std::vector<ModelInfo> infos = LoadInfo(ep, models, r.contextTokens, cloudProviders);

    // Optionally a isSmall model reads the request and names the category (not for images or follow-ups).
    if (settings.autoRouterModel && !c.followUp && c.category != Category::Vision)
    {
        const ModelInfo* router = nullptr;
        for (const auto& m : infos)
        {
            bool quick = m.installed && !m.provider && (m.fit.empty() || m.fit == "gpu") && m.paramsB >= 1.0 && m.paramsB <= 4.5;
            if (quick && (!router || (m.running && !router->running) ||
                             (m.running == router->running && m.paramsB < router->paramsB)))
            {
                router = &m;
            }
        }

        if (router)
        {
            status("Auto: asking " + router->id + " what kind of request this is...");
            std::string tools = req.toolCount ? "yes" : "no";
            json body = { { "model", router->id }, { "stream", false }, { "max_tokens", 8 }, { "temperature", 0 },
                { "chat_template_kwargs", { { "enable_thinking", false } } },
                { "messages",
                    json::array({ { { "role", "system" },
                                      { "content",
                                          "Classify the user's request. Reply with exactly one word:\n"
                                          "quick - a short question or isSmall talk\n"
                                          "general - explanations, writing, summaries, advice\n"
                                          "code - writing, fixing, reviewing or explaining code\n"
                                          "reasoning - maths, logic, planning, careful step-by-step analysis\n"
                                          "agent - it needs actions on the user's files or commands (tools available: " +
                                              tools + ")" } },
                        { { "role", "user" }, { "content", Truncate(req.text, 3000, "...") } } }) } };

            HttpHeaders h = ep.headers;
            h.push_back({ "Content-Type", "application/json" });
            HttpResult res = HttpSend(L"POST", ep.baseUrl + "/chat/completions", body.dump(), h, 180000, {}, cancel);
            json j = json::parse(res.Ok() ? res.body : std::string("{}"), nullptr, false);

            std::string word;
            if (j.is_object() && j.contains("choices") && !j["choices"].empty())
            {
                word = Lower(TrimCopy(j["choices"][0]["message"].value("content", "")));
            }

            Category llm;
            bool parsed = false;
            for (int i = 0; i < kCategoryCount && !parsed; ++i)
            {
                if (word.rfind(kIds[i], 0) == 0)
                {
                    parsed = ParseCategory(kIds[i], llm);
                }
            }

            if (parsed && !(llm == Category::Agent && req.toolCount == 0))
            {
                if (llm != r.category)
                {
                    r.reason = router->id + " says " + CategoryId(llm) + " (the rules said " + CategoryId(r.category) + ")";
                }
                else
                {
                    r.reason += "; " + router->id + " agrees";
                }

                r.category = llm;
            }
        }
    }

    if (cancel && cancel->IsCancelled())
    {
        r.error = "cancelled";
        return r;
    }

    // A model chosen for this kind of request in the Auto mode settings.
    auto ov = settings.autoOverrides.find(CategoryId(r.category));
    if (ov != settings.autoOverrides.end() && std::find(models.begin(), models.end(), ov->second) != models.end())
    {
        r.model = ov->second;
        r.reason += "; " + r.model + " is set for " + CategoryLabel(r.category);
        return r;
    }

    // Otherwise the best score; images fall back to a general model when no vision model is installed.
    Category scoreAs = r.category;
    const ModelInfo* best = nullptr;
    double bestScore = -1e9;
    std::string bestWhy;
    for (int pass = 0; pass < 2 && !best; ++pass)
    {
        for (const auto& m : infos)
        {
            if (!m.installed)
            {
                continue;
            }

            std::string why;
            double s = Score(m, scoreAs, r.contextTokens, &why);
            if (!std::isnan(s) && s > bestScore)
            {
                bestScore = s;
                best = &m;
                bestWhy = why;
            }
        }

        if (!best && scoreAs == Category::Vision)
        {
            scoreAs = Category::General;
            r.reason += "; no vision model is installed";
        }
        else if (!best && scoreAs == Category::Agent)
        {
            scoreAs = Category::Code;  // nothing with tool calling: a coder still helps
            r.reason += "; no installed model can call tools";
        }
    }

    // A model that is not downloaded but would be clearly better (and runs well here): offered for download.
    double suggestScore = -1e9;
    for (const auto& m : infos)
    {
        if (m.installed || !m.fitsDisk || m.fit == "cpu-tight" || m.fit.empty())
        {
            continue;
        }

        std::string why;
        double s = Score(m, scoreAs, r.contextTokens, &why);
        if (!std::isnan(s) && s >= (best ? bestScore : -1e9) + 8 && s > suggestScore)
        {
            suggestScore = s;
            r.suggestion.id = m.id;
            r.suggestion.sizeBytes = m.sizeBytes;
            r.suggestion.why = why + "; better for " + std::string(CategoryLabel(scoreAs)) + " than " +
                               (best ? best->id : std::string("what is installed"));
        }
    }

    if (!best)
    {
        r.error = "Auto mode: no installed model fits this request on this PC" +
                  (r.suggestion.id.empty() ? std::string() : " (" + r.suggestion.id + " would: download it)");
        return r;
    }

    // Stay with the model the conversation used if it is nearly as good: switching costs a model load.
    if (settings.autoSticky && !req.previousModel.empty() && req.previousModel != best->id)
    {
        for (const auto& m : infos)
        {
            if (m.id == req.previousModel)
            {
                std::string why;
                double s = Score(m, scoreAs, r.contextTokens, &why);
                if (!std::isnan(s) && bestScore - s <= 6)
                {
                    best = &m;
                    bestWhy = why.empty() ? "kept from the last answer" : why + ", kept from the last answer";
                }
            }
        }
    }

    r.model = best->id;
    if (!bestWhy.empty())
    {
        r.reason += "; " + r.model + ": " + bestWhy;
    }

    return r;
}

}  // namespace chat
