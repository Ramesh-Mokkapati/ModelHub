#include "HubServer.h"

#include <windows.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "Hash.h"
#include "HttpClient.h"
#include "HubCore.h"
#include "Paths.h"
#include "Text.h"

#ifndef MODELHUB_VERSION
#define MODELHUB_VERSION "0.0.0"
#endif

namespace modelhub {

namespace fs = std::filesystem;
using nlohmann::json;

// The HTTP layer: every /hub/v1 route is a HubCore command (see HubCore.h); /v1 proxies to the model's server.
struct Hub::Impl {
    HubOptions opt;
    std::string token;
    int port = 0;
    std::unique_ptr<HubCore> core;
    httplib::Server http;
    std::thread httpThread;
    std::mutex logMu, waitMu;
    std::condition_variable waitCv;
    bool stopped = false;
    HANDLE instanceMutex = nullptr;

    void Log(const std::string& line) {
        std::lock_guard lk(logMu);
        std::string l = NowIso8601() + " " + line + "\n";
        std::fputs(l.c_str(), stderr);
        try {
            fs::path log = opt.home / L"hub.log";
            std::error_code ec;
            if (fs::exists(log, ec) && fs::file_size(log, ec) > (5u << 20)) fs::rename(log, opt.home / L"hub.log.1", ec);
            AppendTextFile(log, l);
        } catch (...) {
        }
    }

    // A HubCore result as an HTTP response: "status" (if any) becomes the HTTP status.
    static void Reply(httplib::Response& res, json j, int okStatus = 200) {
        int status = okStatus;
        if (j.is_object() && j.contains("status") && j["status"].is_number_integer()) {
            status = j["status"].get<int>();
            j.erase("status");
        }
        res.status = status;
        if (j.is_object() && j.contains("retry_after")) res.set_header("Retry-After", std::to_string(j.value("retry_after", 2)));
        res.set_content(j.dump(), "application/json");
    }
    static void Error(httplib::Response& res, int status, const std::string& msg, const std::string& type = "hub_error") {
        Reply(res, HubCore::Error(status, msg, type));
    }
    static json Body(const httplib::Request& req) {
        json b = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
        return b.is_object() ? b : json::object();
    }

    // OpenAI-compatible inference: start the model's server, then pass the request through (streaming as is).
    void Proxy(const httplib::Request& req, httplib::Response& res, const std::string& path) {
        json body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) return Error(res, 400, "request body must be a JSON object", "invalid_request_error");
        auto cat = core->CatalogSnapshot();
        std::string name = body.value("model", "");
        const CatalogModel* m = cat->Find(name);
        if (!m)
            return Error(res, 404, "unknown model '" + name + "': GET /v1/models lists the installed ones",
                         "invalid_request_error");
        auto inst = core->Store().Find(*m);
        if (!inst)
            return Error(res, 404, "model " + m->id + " is not installed: POST /hub/v1/downloads {\"model\":\"" + m->id + "\"}",
                         "model_not_installed");
        int ctx = 0, parallel = 0;
        if (const TaskProfile* t = cat->FindTask(req.get_header_value("X-ModelHub-Task"))) {
            ctx = t->contextTokens;
            parallel = t->parallel;
        }
        if (std::string c = req.get_header_value("X-ModelHub-Context"); !c.empty()) ctx = std::atoi(c.c_str());
        if (std::string c = req.get_header_value("X-ModelHub-Parallel"); !c.empty()) parallel = std::atoi(c.c_str());
        if (std::string app = req.get_header_value("X-ModelHub-App"); !app.empty()) core->Store().AddUser(*m, app);
        std::string err;
        auto lease = std::make_shared<ServerPool::Lease>(core->Pool().Acquire(*m, inst->path, ctx, parallel, {}, &err));
        if (!*lease) return Error(res, 503, err, "server_unavailable");
        body["model"] = m->id;
        std::string upstream = lease->BaseUrl() + path, payload = body.dump();
        if (body.value("stream", false)) {
            res.set_chunked_content_provider("text/event-stream", [lease, upstream, payload](size_t, httplib::DataSink& sink) {
                HttpResponse r = HttpRequestStream("POST", upstream, payload, {{"Content-Type", "application/json"}},
                                                   30 * 60 * 1000,
                                                   [&](std::string_view chunk) { return sink.write(chunk.data(), chunk.size()); });
                if (!r.Ok() && !r.cancelled) {
                    std::string e = "data: " +
                                    json({{"error", {{"message", r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error}}}}).dump() +
                                    "\n\n";
                    sink.write(e.data(), e.size());
                }
                sink.done();
                return true;
            });
            return;
        }
        HttpResponse r = HttpRequest("POST", upstream, payload, {{"Content-Type", "application/json"}}, 30 * 60 * 1000);
        if (r.status == 0) return Error(res, 502, "llama-server: " + r.error, "server_unavailable");
        res.status = r.status;
        res.set_content(r.body, r.Header("content-type").empty() ? "application/json" : r.Header("content-type").c_str());
    }

    void Routes() {
        http.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            std::string host = ToLower(req.get_header_value("Host"));
            if (auto colon = host.rfind(':'); colon != std::string::npos && host.find(']') == std::string::npos)
                host.resize(colon);
            if (host != "127.0.0.1" && host != "localhost") {  // DNS rebinding from a web page
                Error(res, 403, "ModelHub only answers requests addressed to 127.0.0.1 or localhost");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.path == "/health") return httplib::Server::HandlerResponse::Unhandled;
            if (req.get_header_value("Authorization") != "Bearer " + token) {
                Error(res, 401, "missing or wrong token: read it from hub.json in the ModelHub folder", "authentication_error");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        http.Get("/health", [](const httplib::Request&, httplib::Response& res) { Reply(res, {{"ok", true}}); });
        auto simple = [this](const char* path, const char* method) {
            http.Get(path, [this, method](const httplib::Request&, httplib::Response& res) { Reply(res, core->Call(method, {})); });
        };
        simple("/hub/v1/status", "status");
        simple("/hub/v1/catalog", "catalog");
        simple("/hub/v1/models", "models");
        simple("/hub/v1/downloads", "downloads");
        http.Post("/hub/v1/advise", [this](const httplib::Request& req, httplib::Response& res) { Reply(res, core->Call("advise", Body(req))); });
        http.Post("/hub/v1/fits", [this](const httplib::Request& req, httplib::Response& res) { Reply(res, core->Call("fits", Body(req))); });
        http.Get(R"(/hub/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            Reply(res, core->Call("model", {{"id", req.matches[1].str()}, {"app", req.get_param_value("app")}}));
        });
        // Deletes every copy of a model (Recycle Bin unless ?recycle=false); this hub stops its servers of it first.
        http.Delete(R"(/hub/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            Reply(res, core->Call("delete_model", {{"model", req.matches[1].str()}, {"recycle", req.get_param_value("recycle") != "false"}}));
        });
        http.Post("/hub/v1/downloads", [this](const httplib::Request& req, httplib::Response& res) {
            Reply(res, core->Call("start_download", Body(req)));
        });
        http.Delete(R"(/hub/v1/downloads/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            Reply(res, core->Call("cancel_download", {{"model", req.matches[1].str()}}));
        });
        http.Post("/hub/v1/folders", [this](const httplib::Request& req, httplib::Response& res) {
            json b = Body(req);
            b["app"] = req.get_header_value("X-ModelHub-App");
            Reply(res, core->Call("add_folder", b));
        });
        http.Post("/hub/v1/rescan", [this](const httplib::Request&, httplib::Response& res) { Reply(res, core->Call("rescan", {}), 202); });
        http.Post("/hub/v1/resolve", [this](const httplib::Request& req, httplib::Response& res) { Reply(res, core->Call("resolve_hf", Body(req))); });
        // Hugging Face: search GGUF models, judge one repository's files for this PC, add one of them.
        http.Post("/hub/v1/hf/search", [this](const httplib::Request& req, httplib::Response& res) { Reply(res, core->Call("hf_search", Body(req))); });
        http.Get(R"(/hub/v1/hf/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            Reply(res, core->Call("hf_repo", {{"repo", req.matches[1].str()}}));
        });
        http.Post("/hub/v1/hf/add", [this](const httplib::Request& req, httplib::Response& res) { Reply(res, core->Call("hf_add", Body(req))); });
        http.Post("/hub/v1/custom-models", [this](const httplib::Request& req, httplib::Response& res) { Reply(res, core->Call("add_model", Body(req))); });
        // Loads a model now (waits until its llama-server answers), so the first chat request is not slow.
        http.Post(R"(/hub/v1/servers/(.+)/start)", [this](const httplib::Request& req, httplib::Response& res) {
            json b = Body(req);
            b["model"] = req.matches[1].str();
            b["app"] = req.get_header_value("X-ModelHub-App");
            json r = core->Call("server_start", b);
            if (!r.contains("error")) r["base_url"] = "http://127.0.0.1:" + std::to_string(port) + "/v1";  // through the hub
            Reply(res, r);
        });
        http.Post(R"(/hub/v1/servers/(.+)/stop)", [this](const httplib::Request& req, httplib::Response& res) {
            Reply(res, core->Call("server_stop", {{"model", req.matches[1].str()}}));
        });
        http.Post("/hub/v1/shutdown", [this](const httplib::Request&, httplib::Response& res) {
            Reply(res, {{"state", "stopping"}});
            std::thread([this] { Shutdown(); }).detach();
        });

        http.Get("/v1/models", [this](const httplib::Request&, httplib::Response& res) {
            json data = json::array();
            for (const auto& m : core->CatalogSnapshot()->models)
                if (core->Store().Find(m))
                    data.push_back({{"id", m.id}, {"object", "model"}, {"owned_by", "modelhub"}, {"aliases", m.aliases}});
            Reply(res, {{"object", "list"}, {"data", data}});
        });
        for (const char* p : {"/v1/chat/completions", "/v1/completions", "/v1/embeddings"}) {
            std::string path = p;
            http.Post(p, [this, path](const httplib::Request& req, httplib::Response& res) { Proxy(req, res, path); });
        }
    }

    void Shutdown() {
        http.stop();
        std::lock_guard lk(waitMu);
        stopped = true;
        waitCv.notify_all();
    }
};

Hub::Hub(HubOptions opt) : impl_(std::make_unique<Impl>()) { impl_->opt = std::move(opt); }

Hub::~Hub() {
    Stop();
    if (impl_->httpThread.joinable()) impl_->httpThread.join();
    impl_->core.reset();  // stops the llama-servers
    if (impl_->opt.writeHubInfo) {
        std::error_code ec;
        fs::remove(impl_->opt.home / L"hub.json", ec);
    }
    if (impl_->instanceMutex) CloseHandle(impl_->instanceMutex);
}

int Hub::Port() const { return impl_->port; }
const std::string& Hub::Token() const { return impl_->token; }

bool Hub::Start(std::string* error) {
    Impl& d = *impl_;
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    std::error_code ec;
    fs::create_directories(d.opt.home / L"models", ec);
    if (d.opt.writeHubInfo) {
        std::wstring name = L"Local\\ModelHub-" + Widen(Sha256(ToLower(PathToUtf8(d.opt.home))).substr(0, 16));
        d.instanceMutex = CreateMutexW(nullptr, TRUE, name.c_str());
        if (d.instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS)
            return fail("ModelHub is already running for " + PathToUtf8(d.opt.home));
    }
    CoreOptions co;
    co.home = d.opt.home;
    co.catalogFiles = d.opt.catalogFiles;
    co.config = d.opt.config;
    if (co.config.llamaServer.empty()) {  // bundled: <exe>\llama, or <exe>\..\llama in a multi-config build tree
        co.config.llamaServer = ExeDir() / L"llama" / L"llama-server.exe";
        for (const fs::path& p : {ExeDir().parent_path() / L"llama" / L"llama-server.exe",                      // SDK: bin\<Config>\..
                                  ExeDir().parent_path().parent_path() / L"sdk" / L"bin" / L"llama" / L"llama-server.exe"})  // build tree
            if (!fs::exists(co.config.llamaServer, ec)) co.config.llamaServer = p;
    }
    co.log = [&d](const std::string& s) { d.Log(s); };
    d.core = std::make_unique<HubCore>(co);
    std::string err;
    if (!d.core->Init(&err)) return fail(err);

    d.token = ReplaceAll(NewGuid() + NewGuid(), "-", "");
    d.http.set_read_timeout(60, 0);
    d.http.set_write_timeout(30 * 60, 0);
    d.Routes();
    if (d.opt.config.port > 0 && d.http.bind_to_port("127.0.0.1", d.opt.config.port))
        d.port = d.opt.config.port;
    else
        d.port = d.http.bind_to_any_port("127.0.0.1");
    if (d.port <= 0) return fail("cannot listen on 127.0.0.1");

    if (d.opt.writeHubInfo) {
        wchar_t exe[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        json info = {{"port", d.port},
                     {"url", "http://127.0.0.1:" + std::to_string(d.port)},
                     {"token", d.token},
                     {"pid", GetCurrentProcessId()},
                     {"exe", Narrow(std::wstring(exe, n))},
                     {"version", MODELHUB_VERSION},
                     {"started", NowIso8601()}};
        WriteTextFile(d.opt.home / L"hub.json", info.dump(2) + "\n");
        // The installed hub's location, for clients that need to start it (kept after the hub exits).
        WriteTextFile(d.opt.home / L"install.json", json({{"exe", Narrow(std::wstring(exe, n))}}).dump(2) + "\n");
    }
    auto cat = d.core->CatalogSnapshot();
    d.Log("ModelHub " MODELHUB_VERSION " listening on http://127.0.0.1:" + std::to_string(d.port) + "; " +
          Describe(d.core->Hardware()) + "; catalog " + cat->source + " (" + std::to_string(cat->models.size()) +
          " models, " + std::to_string(cat->tasks.size()) + " tasks)");
    d.httpThread = std::thread([&d] {
        d.http.listen_after_bind();
        std::lock_guard lk(d.waitMu);
        d.stopped = true;
        d.waitCv.notify_all();
    });
    return true;
}

void Hub::Wait() {
    std::unique_lock lk(impl_->waitMu);
    impl_->waitCv.wait(lk, [&] { return impl_->stopped; });
}

void Hub::Stop() {
    if (impl_->http.is_running()) impl_->http.stop();
}

}  // namespace modelhub
