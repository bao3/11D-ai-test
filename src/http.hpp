#pragma once

#include <functional>
#include <string>
#include <vector>

struct ChatMessage {
    std::string role;
    std::string content;
};

struct LlmResponse {
    std::string content;
    double elapsed_s = 0;
    long long tokens = 0;
    double ttft_s = 0;
};

struct ParsedChat {
    std::string content;
    long long tokens = 0;
    bool tokens_from_usage = false;
    bool content_filtered = false;
};

enum class ApiMode {
    Auto,
    Local,
    Compat,
    Gemini,
    OpenAI,
    OpenAIReasoning,
    Anthropic,
    DeepSeek,
    OpenRouter,
    XAI,
};

enum class MaxTokensField {
    Auto,
    MaxTokens,
    MaxCompletionTokens,
    None,
};

enum class TriState {
    Auto,
    Send,
    Omit,
};

struct VertexConfig {
    std::string project;
    std::string location = "global";
    std::string access_token;
    std::string quota_project;
};

// Parse CLI mode token (e.g. "auto", "local", "compat", "gemini", "vertex",
// "openai", "openai-reasoning", "anthropic", "deepseek", "openrouter", "xai",
// "groq", "mistral", "dashscope", "siliconflow", "moonshot", "zhipu", "ollama").
// Throws std::invalid_argument on unknown token.
ApiMode parse_api_mode(const std::string& token);
std::string api_mode_name(ApiMode mode);
bool is_vertex_mode_token(const std::string& token);

MaxTokensField parse_max_tokens_field(const std::string& token);
TriState parse_tri_state(const std::string& token);

// Returns provider default chat/completions URL when user passed a specific
// --api-mode token without --api-url, or empty string if none applies.
std::string default_url_for_mode_token(const std::string& mode_token);

// Builds GCP Vertex AI OpenAI-compatible chat/completions URL for project & location.
std::string build_vertex_chat_completions_url(const std::string& project, const std::string& location = "global");

// Prefixes bare publisher model names (e.g. gemini-3.8-flash -> google/gemini-3.8-flash)
// when targeting Vertex AI's /endpoints/openapi endpoint.
std::string normalize_model_for_endpoint(const std::string& model, const std::string& api_url);

// Resolves GCP Vertex AI project, location, and OAuth access token from env vars,
// Application Default Credentials JSON, or local gcloud CLI.
VertexConfig resolve_vertex_config(const std::string& explicit_project,
                                   const std::string& explicit_location,
                                   bool fetch_token);

// Ordered list of environment variable names to check for an API key.
std::vector<std::string> api_key_env_candidates(ApiMode resolved_mode, const std::string& mode_token);

// Normalizes base URLs ending with /v1, /v1beta/openai, or /endpoints/openapi to .../chat/completions.
std::string normalize_chat_completions_url(const std::string& url);

// Resolves ApiMode::Auto using the target URL (and leaves explicit modes intact).
ApiMode resolve_api_mode(ApiMode configured, const std::string& api_url);

// Throws std::runtime_error when the body is an API error or is not a chat completion.
ParsedChat parse_chat_response(const std::string& body);

class LlmClient {
public:
    std::string api_url;
    std::string api_key;
    std::string model;
    int max_tokens = 65536;
    int timeout_s = 2400;

    ApiMode api_mode = ApiMode::Local;
    MaxTokensField max_tokens_field = MaxTokensField::Auto;
    TriState temperature_policy = TriState::Auto;
    TriState thinking_off_policy = TriState::Auto;
    std::string reasoning_effort;
    int max_retries = 0;
    std::vector<std::string> extra_headers;
    std::function<std::string()> token_refresher;
    mutable std::string refreshed_api_key;

    ApiMode effective_mode() const;
    bool omits_temperature_for_request() const;
    std::string build_request_body(const std::vector<ChatMessage>& messages, double temperature) const;
    LlmResponse complete(const std::vector<ChatMessage>& messages, double temperature) const;
};
