#pragma once
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>

namespace testutil {

// A local "Hugging Face": serves files with HTTP range support (httplib handles Range for set_content).
struct Mirror {
    httplib::Server http;
    std::thread thread;
    int port = 0;
    std::map<std::string, std::string> files;
    std::atomic<int> requests{0};
    std::atomic<int> delayMs{0};  // before answering: keeps a download running
    Mirror() {
        http.Get(R"(/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            ++requests;
            if (int d = delayMs.load()) std::this_thread::sleep_for(std::chrono::milliseconds(d));
            auto it = files.find(req.matches[1].str());
            if (it == files.end()) {
                res.status = 404;
                return;
            }
            res.set_content(it->second, "application/octet-stream");
        });
        port = http.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { http.listen_after_bind(); });
    }
    ~Mirror() {
        http.stop();
        thread.join();
    }
};

}  // namespace testutil
