#include "HfBrowse.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <regex>
#include <sstream>

#include "Advisor.h"
#include "Text.h"

namespace modelhub {

using nlohmann::json;

namespace {

bool Has(const std::string& lower, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (lower.find(w) != std::string::npos) return true;
    return false;
}

// "r1" as its own token: "DeepSeek-R1-Distill", not "llama-3.1-r12"
bool HasToken(const std::string& lower, const std::string& token) {
    for (size_t p = lower.find(token); p != std::string::npos; p = lower.find(token, p + 1)) {
        bool startOk = p == 0 || !std::isalnum(static_cast<unsigned char>(lower[p - 1]));
        size_t e = p + token.size();
        bool endOk = e >= lower.size() || !std::isalnum(static_cast<unsigned char>(lower[e]));
        if (startOk && endOk) return true;
    }
    return false;
}

std::string UrlEncode(const std::string& s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else {
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::vector<std::string> Strings(const json& j) {
    std::vector<std::string> v;
    if (j.is_array())
        for (const auto& x : j)
            if (x.is_string()) v.push_back(x.get<std::string>());
    if (j.is_string()) v.push_back(j.get<std::string>());
    return v;
}

std::string SizeNote(double paramsB) {
    if (paramsB <= 0) return "";
    if (paramsB < 2) return " Small: fast on any PC, best for simple, short tasks.";
    if (paramsB < 5) return " Good quality for its size, and quick even without a strong graphics card.";
    if (paramsB < 10) return " A solid all-rounder; runs well with 8 GB of graphics memory.";
    if (paramsB < 20) return " Strong answers; wants 12 GB or more of graphics memory, or patience on the CPU.";
    return " Among the strongest models you can run locally; needs a lot of memory.";
}

int EstimateLayers(double paramsB) {
    if (paramsB < 1) return 24;
    if (paramsB < 2) return 28;
    if (paramsB < 4) return 36;
    if (paramsB < 10) return 32;
    if (paramsB < 20) return 48;
    if (paramsB < 40) return 64;
    return 80;
}

std::string RepoName(const std::string& repo) {
    std::string n = repo.substr(repo.find('/') + 1);
    for (const char* suffix : {"-GGUF", "-gguf", "_GGUF", "-GGUF-v2"})
        if (n.size() > strlen(suffix) && n.compare(n.size() - strlen(suffix), strlen(suffix), suffix) == 0)
            n.resize(n.size() - strlen(suffix));
    return n;
}

std::string StripMarkdown(std::string s) {
    s = std::regex_replace(s, std::regex(R"(!\[[^\]]*\]\([^)]*\))"), "");          // images
    s = std::regex_replace(s, std::regex(R"(\[([^\]]*)\]\([^)]*\))"), "$1");        // links
    s = std::regex_replace(s, std::regex(R"(<[^>]+>)"), "");                         // html
    s = std::regex_replace(s, std::regex(R"((\*\*|__|`))"), "");                     // emphasis, code
    s = std::regex_replace(s, std::regex(R"(\s+)"), " ");
    return Trim(s);
}

}  // namespace

UseInfo ClassifyUse(const std::string& repo, const std::string& pipelineTag, const std::vector<std::string>& tags,
                    double paramsB, bool tools) {
    const std::string n = ToLower(repo);
    std::vector<std::string> lt;
    for (const auto& t : tags) lt.push_back(ToLower(t));
    auto tag = [&](const char* t) { return std::find(lt.begin(), lt.end(), t) != lt.end(); };
    UseInfo u;
    // Speech, audio and image models are published as GGUF too, but llama-server does not run them.
    static const char* notLanguage[] = {"text-to-speech", "automatic-speech-recognition", "text-to-audio", "audio-to-audio",
                                        "audio-classification", "text-to-image", "image-to-image", "text-to-video",
                                        "image-to-video", "image-classification", "object-detection", "image-segmentation"};
    bool other = std::find(std::begin(notLanguage), std::end(notLanguage), pipelineTag) != std::end(notLanguage) ||
                 HasToken(n, "asr") || HasToken(n, "tts") || Has(n, {"whisper", "parakeet", "music", "audio", "speech", "stable-diffusion",
                         "flux.", "flux-", "sdxl", "ace-step"});
    if (other) {
        u.category = kNotForLlama;
        u.bestFor = "A speech, audio or image model. llama-server, which ModelHub uses, does not run it.";
        return u;
    }
    if (pipelineTag == "feature-extraction" || pipelineTag == "sentence-similarity" ||
        Has(n, {"embed", "bge-", "minilm", "/e5-", "gte-"})) {
        u.category = "Embeddings";
        u.bestFor = "Turns text into vectors for similarity search and retrieval (RAG). Not a chat model.";
        return u;
    }
    if (pipelineTag == "image-text-to-text" || Has(n, {"-vl", "_vl", "vision", "llava", "pixtral"})) {
        u.category = "Vision + text";
        u.bestFor = "Answers questions about images as well as text. ModelHub runs its text part; images need its "
                    "separate mmproj file, which ModelHub does not load yet.";
    } else if (Has(n, {"coder", "starcoder", "codellama", "codegemma", "devstral", "codestral", "deepseek-code"}) ||
               tag("code")) {
        u.category = "Coding";
        u.bestFor = "Writing, explaining, reviewing and converting code: a programming assistant.";
    } else if (HasToken(n, "r1") || Has(n, {"qwq", "reason", "thinking", "-think"})) {
        u.category = "Reasoning";
        u.bestFor = "Step-by-step reasoning for maths, logic and planning. It thinks before it answers, so replies "
                    "are slower and longer.";
    } else if (Has(n, {"math"})) {
        u.category = "Math";
        u.bestFor = "Maths problems and calculations, shown step by step.";
    } else if (!Has(n, {"instruct", "chat", "-it", "_it", "it-", "assistant"}) && Has(n, {"base", "-pt"})) {
        u.category = "Base model";
        u.bestFor = "Raw text completion. It is not tuned to follow instructions, so it needs careful prompts or "
                    "fine-tuning.";
    } else {
        u.category = "Chat and writing";
        u.bestFor = "A general assistant: questions, summaries, writing, translation and brainstorming.";
    }
    u.bestFor += SizeNote(paramsB);
    if (tools) u.bestFor += " Supports tool calling.";
    return u;
}

QuantInfo ParseQuant(const std::string& fileName) {
    std::string up;
    for (char c : fileName) up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const std::regex re(R"((IQ[1-4]_(XXS|XS|S|M|NL)|Q[2-8]_K(_(S|M|L|XL))?|Q4_0_[48]_[48]|Q[4-8]_[01]|BF16|F16|F32|MXFP4|TQ[12]_0))");
    QuantInfo q;
    for (auto it = std::sregex_iterator(up.begin(), up.end(), re); it != std::sregex_iterator(); ++it) q.name = (*it)[1];
    const std::string& s = q.name;
    if (s.empty()) {
        q.quality = "Unknown quantisation";
        q.rank = 4;
    } else if (s == "F32" || s == "F16" || s == "BF16") {
        q.quality = "Original precision: the largest file, no quality loss";
        q.rank = 8;
    } else if (s == "Q8_0") {
        q.quality = "Near-original quality";
        q.rank = 7;
    } else if (s.rfind("Q6", 0) == 0) {
        q.quality = "Very high quality";
        q.rank = 6;
    } else if (s.rfind("Q5", 0) == 0) {
        q.quality = "High quality";
        q.rank = 5;
    } else if (s.rfind("Q4_0_", 0) == 0) {
        q.quality = "For ARM processors only";
        q.rank = 4;
        q.armOnly = true;
    } else if (s == "Q4_K_M") {
        q.quality = "Recommended balance of size and quality";
        q.rank = 4.5;
    } else if (s.rfind("Q4", 0) == 0 || s.rfind("IQ4", 0) == 0 || s == "MXFP4") {
        q.quality = "Good quality";
        q.rank = 4;
    } else if (s.rfind("Q3", 0) == 0 || s.rfind("IQ3", 0) == 0) {
        q.quality = "Lower quality: only if memory is tight";
        q.rank = 3;
    } else {
        q.quality = "Much lower quality";
        q.rank = 2;
    }
    return q;
}

std::string CardParagraph(const std::string& markdown, size_t maxChars) {
    std::string md = markdown;
    if (md.rfind("---", 0) == 0)  // YAML front matter
        if (auto end = md.find("\n---", 3); end != std::string::npos) md = md.substr(md.find('\n', end + 1) + 1);
    std::istringstream in(md);
    std::string line, para;
    bool fence = false;
    std::vector<std::string> paras;
    auto flush = [&] {
        std::string p = StripMarkdown(para);
        if (!p.empty()) paras.push_back(p);
        para.clear();
    };
    while (std::getline(in, line)) {
        std::string t = Trim(line);
        if (t.rfind("```", 0) == 0) {
            fence = !fence;
            flush();
            continue;
        }
        if (fence) continue;
        if (t.empty() || t[0] == '#' || t[0] == '|' || t[0] == '>' || t.rfind("- ", 0) == 0 || t.rfind("* ", 0) == 0 ||
            t.rfind("<", 0) == 0 || t.rfind("![", 0) == 0 || t.rfind("[!", 0) == 0) {
            flush();
            continue;
        }
        para += " " + t;
    }
    flush();
    for (const auto& p : paras) {
        const std::string l = ToLower(p);
        if (p.size() < 60 || Has(l, {"quant", "gguf", "imatrix", "llama.cpp", "lm studio", "ollama", "original model:",
                                     "made using", "huggingface-cli", "download", "license", "citation", "@misc", "metal",
                                     "arm chips", "offload", "repack", "*not*", "q4_0", "q8_0", "bpw"}))
            continue;
        std::string q = p;
        if (!q.empty() && q.back() == ':') {
            size_t end = q.rfind(". ");
            if (end == std::string::npos) continue;
            q.resize(end + 1);
        }
        if (q.size() <= maxChars) return q;
        const std::string& p2 = q;
        size_t cut = p2.rfind(". ", maxChars);
        return cut != std::string::npos && cut > maxChars / 3 ? p2.substr(0, cut + 1) : p2.substr(0, maxChars) + "...";
    }
    return "";
}

double ParamsFromName(const std::string& name, double* activeB) {
    static const std::regex moe(R"((\d+(?:\.\d+)?)[bB]-[aA](\d+(?:\.\d+)?)[bB])");
    static const std::regex dense(R"((?:^|[-_/\s])(\d+(?:\.\d+)?)[bB](?:$|[-_\s.]))");
    std::smatch m;
    if (std::regex_search(name, m, moe)) {
        if (activeB) *activeB = std::stod(m[2]);
        return std::stod(m[1]);
    }
    if (std::regex_search(name, m, dense)) {
        double p = std::stod(m[1]);
        if (activeB) *activeB = p;
        return p;
    }
    if (activeB) *activeB = 0;
    return 0;
}

CatalogModel EstimatedModel(const std::string& id, uint64_t sizeBytes, double paramsB, double activeB, int contextK) {
    CatalogModel m;
    m.id = id;
    m.name = id;
    CatalogFile f;
    f.file = id + ".gguf";
    f.size = sizeBytes;
    m.files.push_back(f);
    if (paramsB <= 0) paramsB = std::max(0.1, sizeBytes / double(1ull << 30) * 1.7);  // ~Q4_K_M density
    if (activeB <= 0 || activeB > paramsB) activeB = paramsB;
    m.paramsB = paramsB;
    m.activeParamsB = activeB;
    m.layers = EstimateLayers(paramsB);
    m.contextK = contextK > 0 ? contextK : 8;
    // KV cache of one 8K context: ~96 MB per billion parameters for dense models with grouped-query attention;
    // mixture-of-experts models have far less attention than their total size suggests.
    double basis = activeB < paramsB ? std::sqrt(paramsB * activeB) : paramsB;
    m.kvCacheMB8k = static_cast<uint64_t>(std::clamp(basis * 96.0, 32.0, 4096.0));
    return m;
}

std::string HfSearchUrl(const std::string& base, const std::string& query, const std::string& kind,
                        const std::string& sort, int limit) {
    std::string url = base + "/api/models?filter=gguf";
    if (kind == "code") url += "&filter=code";
    else if (kind == "text") url += "&pipeline_tag=text-generation";
    else if (kind == "embeddings") url += "&pipeline_tag=feature-extraction";
    if (!Trim(query).empty()) url += "&search=" + UrlEncode(Trim(query));
    const std::string s = sort == "likes" ? "likes" : sort == "updated" ? "lastModified" : sort == "trending" ? "trendingScore" : "downloads";
    url += "&sort=" + s + "&direction=-1&limit=" + std::to_string(std::clamp(limit, 1, 100));
    for (const char* e : {"gguf", "cardData", "pipeline_tag", "downloads", "likes", "tags", "gated", "lastModified"})
        url += std::string("&expand[]=") + e;
    return url;
}

std::string HfModelUrl(const std::string& base, const std::string& repo) {
    std::string url = base + "/api/models/" + repo + "?";
    bool first = true;
    for (const char* e : {"gguf", "cardData", "pipeline_tag", "downloads", "likes", "tags", "gated", "lastModified"}) {
        url += std::string(first ? "" : "&") + "expand[]=" + e;
        first = false;
    }
    return url;
}

json SummarizeSearchItem(const json& item, const HardwareInfo& hw) {
    const std::string repo = item.value("id", "");
    const json gguf = item.value("gguf", json::object());
    const json card = item.value("cardData", json::object());
    double active = 0;
    double params = gguf.is_object() && gguf.value("total", 0.0) > 0 ? gguf.value("total", 0.0) / 1e9 : 0;
    double named = ParamsFromName(repo, &active);
    if (params <= 0) params = named;
    if (active <= 0 || active >= params) active = params;
    const bool tools = gguf.is_object() && gguf.value("chat_template", "").find("tools") != std::string::npos;
    const std::string pipeline = item.value("pipeline_tag", card.is_object() ? card.value("pipeline_tag", "") : "");
    UseInfo use = ClassifyUse(repo, pipeline, Strings(item.value("tags", json::array())), params, tools);
    json gated = item.value("gated", json(false));
    json j = {{"repo", repo},
              {"downloads", item.value("downloads", 0)},
              {"likes", item.value("likes", 0)},
              {"updated", item.value("lastModified", "")},
              {"pipeline", pipeline},
              {"architecture", gguf.is_object() ? gguf.value("architecture", "") : ""},
              {"params_b", std::round(params * 10) / 10},
              {"active_params_b", std::round(active * 10) / 10},
              {"context_k", gguf.is_object() ? gguf.value("context_length", 0) / 1024 : 0},
              {"gated", !(gated.is_boolean() && !gated.get<bool>())},
              {"license", card.is_object() && card.contains("license") ? Strings(card["license"]).empty() ? "" : Strings(card["license"])[0] : ""},
              {"category", use.category},
              {"best_for", use.bestFor},
              {"tools", tools},
              {"language_model", use.category != kNotForLlama}};
    if (params > 0 && use.category != kNotForLlama) {  // how a typical Q4_K_M file of it would run here
        uint64_t size = static_cast<uint64_t>(params * 1e9 * 4.85 / 8);
        CatalogModel m = EstimatedModel(repo, size, params, active, 8);
        Fit f = FitFor(m, hw, 8192, 1, Engine::Hub);
        j["q4_size"] = size;
        j["fit"] = ToString(f);
        j["speed"] = SpeedLabel(m, hw, f);
    }
    return j;
}

json EvaluateRepo(const std::string& base, const std::string& repo, const json& info, const json& tree,
                  const std::string& card, const std::string& baseCard, const HardwareInfo& hw, const KnownFileFn& known) {
    json j = SummarizeSearchItem(info.is_object() ? info : json::object(), hw);
    j["repo"] = repo;
    j["url"] = base + "/" + repo;
    const json cd = info.is_object() ? info.value("cardData", json::object()) : json::object();
    std::vector<std::string> baseModels = cd.is_object() ? Strings(cd.value("base_model", json())) : std::vector<std::string>{};
    j["base_model"] = baseModels.empty() ? "" : baseModels[0];
    std::string description = CardParagraph(card);
    if (description.empty()) description = CardParagraph(baseCard);
    j["description"] = description;

    const double params = j.value("params_b", 0.0), active = j.value("active_params_b", 0.0);
    const int contextK = j.value("context_k", 0);
    const bool gated = j.value("gated", false);
    json notes = json::array();
    if (j.value("category", "") == kNotForLlama)
        notes.push_back("This is not a language model: llama-server, which ModelHub uses, cannot run it.");
    if (gated) notes.push_back("Gated: downloading needs a Hugging Face account that accepted the licence, which ModelHub does not support yet.");
    if (!hw.avx2 && hw.vramMB == 0) notes.push_back("This processor lacks AVX2, so llama.cpp runs slowly on it.");
    if (j.value("category", "") == "Vision + text") notes.push_back("Images need the mmproj file; ModelHub runs the text part only.");
    if (params <= 0) notes.push_back("The model's size is not published; how it runs here is estimated from the file size.");

    json files = json::array();
    bool folders = false;
    static const std::regex split(R"(-(\d{5})-of-(\d{5})\.gguf$)", std::regex::icase);
    std::map<std::string, uint64_t> splitTotal;  // "name-of-00003.gguf" -> the size of all its parts
    if (tree.is_array())
        for (const auto& e : tree) {
            const std::string path = e.value("path", "");
            std::smatch sm;
            if (path.find('/') == std::string::npos && std::regex_search(path, sm, split))
                splitTotal[path.substr(0, sm.position(0)) + "-of-" + sm[2].str()] += e.value("size", uint64_t{0});
        }
    if (tree.is_array())
        for (const auto& e : tree) {
            const std::string path = e.value("path", "");
            if (e.value("type", "") == "directory") {
                folders = true;
                continue;
            }
            if (!EndsWithI(path, ".gguf") || ToLower(path).find("mmproj") != std::string::npos) continue;
            if (path.find('/') != std::string::npos) {
                folders = true;
                continue;
            }
            std::smatch sm;
            bool isSplit = std::regex_search(path, sm, split);
            if (isSplit && sm[1] != "00001") continue;  // one row per split model
            const uint64_t size = isSplit ? splitTotal[path.substr(0, sm.position(0)) + "-of-" + sm[2].str()]
                                          : e.value("size", uint64_t{0});
            std::string sha;
            if (auto lfs = e.value("lfs", json::object()); lfs.is_object()) sha = ToLower(lfs.value("oid", ""));
            QuantInfo q = ParseQuant(path);
            CatalogModel m = EstimatedModel(ToLower(path.substr(0, path.size() - 5)), size, params, active, contextK);
            Fit fit = FitFor(m, hw, 8192, 1, Engine::Hub);
            json f = {{"file", path},
                      {"size", size},
                      {"sha256", sha},
                      {"url", base + "/" + repo + "/resolve/main/" + path},
                      {"quant", q.name},
                      {"quality", q.quality},
                      {"rank", q.rank},
                      {"fit", ToString(fit)},
                      {"needs_mb", NeedFor(m, 8192, 1).totalMB},
                      {"speed", SpeedLabel(m, hw, fit)},
                      {"recommended", false}};
            std::string why;
            if (gated) why = "Gated repository";
            else if (j.value("category", "") == kNotForLlama) why = "Not a language model";
            else if (isSplit) why = "Split into parts: not supported yet";
            else if (q.armOnly) why = "For ARM processors; this PC is x64";
            else if (sha.size() != 64) why = "No published SHA-256";
            else if (fit == Fit::TooLarge) why = "Too large for this PC";
            f["compatible"] = why.empty();
            f["reason"] = why;
            if (known && sha.size() == 64) {
                KnownFile k = known(sha);
                f["installed"] = k.installed;
                f["catalog_id"] = k.catalogId;
            }
            files.push_back(f);
        }
    if (folders && files.empty())
        notes.push_back("This repository keeps its files in folders (split models), which ModelHub cannot download yet.");
    std::sort(files.begin(), files.end(), [](const json& a, const json& b) { return a["size"].get<uint64_t>() < b["size"].get<uint64_t>(); });

    // Recommend: the best quality that runs comfortably (fully on the graphics card, or on the CPU without one),
    // up to Q8_0 on a graphics card and Q5 on the CPU (bigger is slower there); else the closest to Q4_K_M that
    // runs at all; else the smallest file that runs.
    const bool gpu = hw.vramMB > 0;
    const std::string comfortable = gpu ? "gpu" : "cpu";
    const double cap = gpu ? 7 : 5;
    int pick = -1;
    auto better = [&](int i, int best) {
        if (best < 0) return true;
        double ri = files[i]["rank"], rb = files[best]["rank"];
        if (!gpu && ri != rb && (ri == 4.5 || rb == 4.5)) return ri == 4.5;  // on the CPU, Q4_K_M first
        return ri > rb;
    };
    for (int i = 0; i < static_cast<int>(files.size()); ++i)
        if (files[i]["compatible"] && files[i]["fit"] == comfortable && files[i]["rank"].get<double>() >= 4 &&
            files[i]["rank"].get<double>() <= cap && better(i, pick))
            pick = i;
    if (pick < 0)
        for (int i = 0; i < static_cast<int>(files.size()); ++i)
            if (files[i]["compatible"]) {
                double d = std::abs(files[i]["rank"].get<double>() - 4.5);
                if (pick < 0 || d < std::abs(files[pick]["rank"].get<double>() - 4.5)) pick = i;
            }
    if (pick >= 0) {
        files[pick]["recommended"] = true;
        j["recommended"] = files[pick]["file"];
    } else {
        j["recommended"] = "";
        if (!files.empty() && !gated) notes.push_back("None of its files runs on this PC.");
    }
    int compatible = 0;
    for (const auto& f : files) compatible += f["compatible"].get<bool>();
    j["compatible_files"] = compatible;
    j["files"] = files;
    j["notes"] = notes;
    return j;
}

std::optional<json> CustomModelEntry(const json& repo, const std::string& file, std::string* error) {
    for (const auto& f : repo.value("files", json::array())) {
        if (f.value("file", "") != file) continue;
        if (!f.value("compatible", false)) {
            if (error) *error = file + ": " + f.value("reason", "not compatible");
            return std::nullopt;
        }
        const std::string r = repo.value("repo", "");
        const std::string quant = f.value("quant", "");
        std::string summary = repo.value("best_for", "");
        if (!repo.value("description", "").empty()) summary += " " + repo.value("description", "");
        summary += " From huggingface.co/" + r + ".";
        double params = repo.value("params_b", 0.0), active = repo.value("active_params_b", 0.0);
        CatalogModel m = EstimatedModel(ToLower(file.substr(0, file.size() - 5)), f.value("size", uint64_t{0}), params,
                                        active, repo.value("context_k", 0));
        json e = {{"id", m.id},
                  {"name", RepoName(r) + (quant.empty() ? "" : " (" + quant + ")")},
                  {"family", repo.value("architecture", "")},
                  {"file", file},
                  {"url", f.value("url", "")},
                  {"size", f.value("size", uint64_t{0})},
                  {"sha256", f.value("sha256", "")},
                  {"params_b", m.paramsB},
                  {"active_params_b", m.activeParamsB},
                  {"layers", m.layers},
                  {"context_k", m.contextK},
                  {"kv_cache_mb_8k", m.kvCacheMB8k},
                  {"tools", repo.value("tools", false)},
                  {"embeddings", repo.value("category", "") == "Embeddings"},
                  {"license", repo.value("license", "")},
                  {"summary", summary}};
        return e;
    }
    if (error) *error = file + " is not in " + repo.value("repo", "");
    return std::nullopt;
}

}  // namespace modelhub
