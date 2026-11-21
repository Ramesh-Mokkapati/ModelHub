#pragma once
#include "stdafx.h"

#include "Http.h"
#include "LlmClient.h"
#include "Store.h"

namespace chat
{

// Auto mode: for each message, what kind of request it is, and which installed model suits it best on this PC.
// The ModelHub service then loads that model (and frees graphics memory from idle ones) on the first request.
enum class Category
{
    Quick,      // short questions and small talk: the fastest good model
    General,    // explanations, writing, summaries
    Code,       // writing, fixing or explaining code
    Reasoning,  // maths, logic, planning, careful analysis
    Agent,      // work on the workspace through tools (files, search, shell)
    Vision      // an image is attached
};
constexpr int kCategoryCount = 6;
const char* CategoryId(Category c);      // "quick", "general", ...
const char* CategoryLabel(Category c);   // "Quick questions", ...
bool ParseCategory(const std::string& id, Category& out);

struct RouteRequest
{
    std::string text;                   // the new message (without inlined attachments)
    std::vector<std::string> attachmentNames;
    bool hasImages = false;
    size_t promptChars = 0;             // everything that will be sent: system, history, attachments
    size_t toolCount = 0;               // tools offered to the model
    std::string previousModel;          // the model of the last answer in this conversation, if any
    std::string previousCategory;
};

struct Classification
{
    Category category = Category::General;
    std::string reason;
    int estimatedTokens = 0;
    bool followUp = false;
};

// Rules on the text, the attachments and the conversation: no model needed.
Classification Classify(const RouteRequest& req);

struct ModelInfo
{
    std::string id;
    int tier = 1;
    double paramsB = 0;
    double activeParamsB = 0;
    bool tools = true;
    int contextK = 32;
    std::string fit;    // gpu, gpu+cpu, cpu, cpu-tight, too-large; "" when unknown (not ModelHub)
    std::string speed;  // fast, moderate, slow; "" when unknown
    bool running = false;
    bool vision = false;
    bool installed = true;
    bool fitsDisk = true;
    unsigned long long sizeBytes = 0;
    bool provider = false;   // "<provider>|<model>": another app on this PC or a cloud API
    bool cloud = false;
    double quality = 0;      // set for cloud models (their size is not published)
};

// A catalog model that is not downloaded yet but would suit this request clearly better.
struct ModelSuggestion
{
    std::string id;
    unsigned long long sizeBytes = 0;
    std::string why;  // "fast on gpu+cpu, better at code"
};

struct RouteResult
{
    std::string model;
    Category category = Category::General;
    std::string reason;      // for the transcript: why this category and this model
    int contextTokens = 0;   // the context the request needs (for X-ModelHub-Context)
    std::string error;
    ModelSuggestion suggestion;  // id empty: none
};

// models: the chat models Auto mode may use: ModelHub's installed ones, and "<provider>|<model>" for providers the
// user allowed in Auto mode (cloud is a set of their ids). The combo box list. status gets progress lines ("asking ... to classify").
RouteResult Route(const Endpoint& ep, const Settings& settings, const RouteRequest& req,
    const std::vector<std::string>& models, CancelToken* cancel, const std::function<void(const std::string&)>& status,
    const std::set<std::string>& cloudProviders = {});

}  // namespace chat
