#pragma once
#include "stdafx.h"

#include "Http.h"

namespace chat
{

struct ToolContext
{
    std::wstring workspace;  // relative paths resolve here
    std::string shell = "cmd";
    bool confirmShell = true;
    bool confirmWrites = true;
    CancelToken* cancel = nullptr;
    // Asks the user (title, details); false: declined. Called on the worker thread.
    std::function<bool(const std::string&, const std::string&)> confirm;
};

const std::vector<std::string>& BuiltinToolNames();
bool IsBuiltinTool(const std::string& name);
json BuiltinToolDefinition(const std::string& name);  // {type:"function", function:{name, description, parameters}}
std::string RunBuiltinTool(const std::string& name, const json& args, const ToolContext& ctx);

}  // namespace chat
