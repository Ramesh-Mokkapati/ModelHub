#pragma once
#include "stdafx.h"

#include "Http.h"
#include "Store.h"

namespace chat
{

struct McpTool
{
    std::string name;
    std::string description;
    json inputSchema;
};

struct McpResource
{
    std::string uri;
    std::string name;
    std::string description;
    std::string mimeType;
};

// One MCP server over stdio (newline-delimited JSON-RPC) or streamable HTTP.
class McpServer
{
public:
    struct Info
    {
        McpServerConfig config;
        std::string state;  // "off", "connecting", "connected", "error: ..."
        std::vector<McpTool> tools;
        std::vector<McpResource> resources;
    };

    explicit McpServer(McpServerConfig config);
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    bool Connect();     // initialize, tools/list, resources/list; blocking
    void Disconnect();  // safe from any thread
    Info Snapshot() const;
    const McpServerConfig& Config() const
    {
        return config_;
    }

    // tools/call: the text of the result; isError when the server reports a failure.
    std::string CallTool(const std::string& tool, const json& args, CancelToken* cancel, bool& isError);
    // resources/read: the text of every content item.
    std::string ReadResource(const std::string& uri, std::string& error);

private:
    json Request(const std::string& method, const json& params, DWORD timeoutMs, std::string& error,
        CancelToken* cancel = nullptr);
    void Notify(const std::string& method, const json& params);
    bool StartProcess(std::string& error);
    void ReaderLoop();
    void Dispatch(const json& msg);
    bool WriteLine(const std::string& line);
    json HttpRpc(const json& msg, DWORD timeoutMs, std::string& error, CancelToken* cancel);
    void SetState(const std::string& s);

    McpServerConfig config_;
    mutable std::mutex dataMu_;
    std::string state_ = "off";
    std::vector<McpTool> tools_;
    std::vector<McpResource> resources_;

    std::mutex lifeMu_;  // Connect / Disconnect
    std::mutex procMu_;
    HANDLE process_ = nullptr, stdinWrite_ = nullptr, stdoutRead_ = nullptr, job_ = nullptr;
    std::thread reader_;
    std::mutex writeMu_;
    std::mutex rpcMu_;
    std::condition_variable rpcCv_;
    std::map<long long, json> responses_;
    bool readerDone_ = true;
    std::atomic<long long> nextId_{ 1 };
    CancelToken connectCancel_;
    std::string sessionId_, protocolVersion_;
};

// The configured servers. Tools reach the model as "<server>__<tool>".
class McpManager
{
public:
    ~McpManager();
    // Keeps servers whose settings did not change; starts the new ones (enabled) on background threads.
    // notify gets WM_APP_MCP_CHANGED when a server's state changes.
    void Apply(const std::vector<McpServerConfig>& configs, HWND notify);
    void Reconnect(HWND notify);
    void Shutdown();
    std::vector<std::shared_ptr<McpServer>> Servers() const;

    static std::string QualifiedName(const std::string& server, const std::string& tool);

private:
    void ConnectAsync(const std::shared_ptr<McpServer>& s, HWND notify);
    mutable std::mutex mu_;
    std::vector<std::shared_ptr<McpServer>> servers_;
};

}  // namespace chat
