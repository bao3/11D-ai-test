#include "http.hpp"

#include "utf8.hpp"

#include <nlohmann/json.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

void ensure_curl() {
    static CurlGlobal global;
    (void)global;
}

constexpr std::size_t kMaxHttpBody = 64ull * 1024ull * 1024ull;

std::size_t write_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const std::size_t chunk = size * nmemb;
    if (out->size() + chunk > kMaxHttpBody) {
        // Returning less than requested aborts the transfer (CURLE_WRITE_ERROR).
        return 0;
    }
    out->append(ptr, chunk);
    return chunk;
}

std::string ascii_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool starts_with(const std::string& s, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::size_t header_callback(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
    const std::size_t total = size * nitems;
    auto* retry_after_s = static_cast<int*>(userdata);
    if (retry_after_s != nullptr && total > 12) {
        std::string line(buffer, total);
        const std::string lower = ascii_lower(line);
        if (starts_with(lower, "retry-after:")) {
            const std::string val = trim_copy(line.substr(12));
            try {
                const int parsed = std::stoi(val);
                if (parsed > 0) {
                    *retry_after_s = parsed;
                }
            } catch (const std::exception&) {
            }
        }
    }
    return total;
}

std::string snippet(const std::string& text, std::size_t n = 400) {
    if (text.size() <= n) {
        return text;
    }
    return text.substr(0, n);
}

long long estimate_tokens(const std::string& content) {
    const auto chars = static_cast<long long>(utf8_len(content));
    return std::max<long long>(1, chars / 2);
}

bool is_openai_reasoning_series(const std::string& raw_model, bool require_openai_prefix_or_exact) {
    std::string m = ascii_lower(trim_copy(raw_model));
    bool had_openai_ns = false;
    if (starts_with(m, "openai/")) {
        had_openai_ns = true;
        m = m.substr(7);
    } else if (m.find('/') != std::string::npos && require_openai_prefix_or_exact) {
        return false;
    }
    auto match_family = [&](const char* fam) {
        const std::size_t n = std::char_traits<char>::length(fam);
        return m.size() >= n && m.compare(0, n, fam) == 0 && (m.size() == n || m[n] == '-');
    };
    if (match_family("o1") || match_family("o3") || match_family("o4")) {
        return true;
    }
    if (match_family("gpt-5") && !starts_with(m, "gpt-5-chat")) {
        return !require_openai_prefix_or_exact || had_openai_ns || raw_model.find('/') == std::string::npos;
    }
    return false;
}

bool is_retryable_http(long code) {
    return code == 408 || code == 429 || code == 500 || code == 502 || code == 503 || code == 504 || code == 529;
}

bool is_retryable_curl(CURLcode rc) {
    return rc == CURLE_GOT_NOTHING || rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR ||
           rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_PARTIAL_FILE;
}

std::string first_non_empty_env(const std::vector<const char*>& names) {
    for (const char* name : names) {
        if (const char* val = std::getenv(name)) {
            const std::string t = trim_copy(val);
            if (!t.empty() && t != "(unset)") {
                return t;
            }
        }
    }
    return {};
}

std::string run_cmd_capture(const std::vector<std::string>& args) {
    if (args.empty()) {
        return {};
    }
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return {};
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return {};
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        // Silence stderr from gcloud warnings so stdout stays clean.
        if (FILE* devnull = std::fopen("/dev/null", "w")) {
            dup2(fileno(devnull), STDERR_FILENO);
            std::fclose(devnull);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(pipefd[1]);
    std::string out;
    char buf[512];
    for (;;) {
        const ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (out.size() + static_cast<std::size_t>(n) <= 16384) {
            out.append(buf, static_cast<std::size_t>(n));
        }
    }
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return {};
    }
    const std::string trimmed = trim_copy(out);
    if (trimmed == "(unset)") {
        return {};
    }
    return trimmed;
}

std::string default_adc_path() {
    if (const char* gac = std::getenv("GOOGLE_APPLICATION_CREDENTIALS")) {
        const std::string p = trim_copy(gac);
        if (!p.empty()) {
            return p;
        }
    }
    if (const char* home = std::getenv("HOME")) {
        const std::string h = trim_copy(home);
        if (!h.empty()) {
            return h + "/.config/gcloud/application_default_credentials.json";
        }
    }
    return {};
}

bool load_json_file(const std::string& path, nlohmann::json& out) {
    if (path.empty()) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    try {
        in >> out;
        return out.is_object();
    } catch (const std::exception&) {
        return false;
    }
}

std::string exchange_adc_refresh_token(const nlohmann::json& adc) {
    if (!adc.is_object()) {
        return {};
    }
    if (!adc.contains("client_id") || !adc.contains("client_secret") || !adc.contains("refresh_token") ||
        !adc["client_id"].is_string() || !adc["client_secret"].is_string() || !adc["refresh_token"].is_string()) {
        return {};
    }
    ensure_curl();
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return {};
    }
    struct CurlGuard {
        CURL* h;
        ~CurlGuard() { curl_easy_cleanup(h); }
    } guard{curl};

    auto esc = [&](const std::string& s) {
        char* raw = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
        if (raw == nullptr) {
            return std::string();
        }
        std::string res(raw);
        curl_free(raw);
        return res;
    };
    std::string token_uri = "https://oauth2.googleapis.com/token";
    if (adc.contains("token_uri") && adc["token_uri"].is_string() && !adc["token_uri"].get<std::string>().empty()) {
        token_uri = adc["token_uri"].get<std::string>();
    }
    const std::string form = "grant_type=refresh_token&client_id=" + esc(adc["client_id"].get<std::string>()) +
                             "&client_secret=" + esc(adc["client_secret"].get<std::string>()) +
                             "&refresh_token=" + esc(adc["refresh_token"].get<std::string>());

    std::string resp;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Accept: application/json");
    struct HeaderGuard {
        curl_slist* list;
        ~HeaderGuard() { curl_slist_free_all(list); }
    } hg{headers};

    curl_easy_setopt(curl, CURLOPT_URL, token_uri.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(form.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    if (curl_easy_perform(curl) != CURLE_OK) {
        return {};
    }
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (code < 200 || code >= 300) {
        return {};
    }
    try {
        const auto j = nlohmann::json::parse(resp);
        if (j.contains("access_token") && j["access_token"].is_string()) {
            return trim_copy(j["access_token"].get<std::string>());
        }
    } catch (const std::exception&) {
    }
    return {};
}

}  // namespace

bool is_vertex_mode_token(const std::string& token) {
    const std::string m = ascii_lower(trim_copy(token));
    return m == "vertex" || m == "vertex-ai" || m == "vertexai" || m == "gcp";
}

ApiMode parse_api_mode(const std::string& token) {
    const std::string m = ascii_lower(trim_copy(token));
    if (m == "auto") {
        return ApiMode::Auto;
    }
    if (m == "local" || m == "ps5ai") {
        return ApiMode::Local;
    }
    if (m == "compat" || m == "standard" || m == "openai-compat" || m == "cloud" || m == "ollama" || m == "vllm" ||
        m == "groq" || m == "mistral" || m == "dashscope" || m == "qwen" || m == "siliconflow" || m == "moonshot" ||
        m == "kimi" || m == "zhipu" || m == "glm" || m == "together" || m == "fireworks") {
        return ApiMode::Compat;
    }
    if (m == "gemini" || m == "google" || is_vertex_mode_token(m)) {
        return ApiMode::Gemini;
    }
    if (m == "openai" || m == "azure" || m == "azure-openai") {
        return ApiMode::OpenAI;
    }
    if (m == "openai-reasoning") {
        return ApiMode::OpenAIReasoning;
    }
    if (m == "anthropic" || m == "claude") {
        return ApiMode::Anthropic;
    }
    if (m == "deepseek") {
        return ApiMode::DeepSeek;
    }
    if (m == "openrouter") {
        return ApiMode::OpenRouter;
    }
    if (m == "xai" || m == "grok") {
        return ApiMode::XAI;
    }
    throw std::invalid_argument("unknown --api-mode: " + token);
}

std::string api_mode_name(ApiMode mode) {
    switch (mode) {
        case ApiMode::Auto:
            return "auto";
        case ApiMode::Local:
            return "local";
        case ApiMode::Compat:
            return "compat";
        case ApiMode::Gemini:
            return "gemini";
        case ApiMode::OpenAI:
            return "openai";
        case ApiMode::OpenAIReasoning:
            return "openai-reasoning";
        case ApiMode::Anthropic:
            return "anthropic";
        case ApiMode::DeepSeek:
            return "deepseek";
        case ApiMode::OpenRouter:
            return "openrouter";
        case ApiMode::XAI:
            return "xai";
    }
    return "local";
}

MaxTokensField parse_max_tokens_field(const std::string& token) {
    const std::string t = ascii_lower(trim_copy(token));
    if (t == "auto") {
        return MaxTokensField::Auto;
    }
    if (t == "max_tokens" || t == "max-tokens") {
        return MaxTokensField::MaxTokens;
    }
    if (t == "max_completion_tokens" || t == "max-completion-tokens") {
        return MaxTokensField::MaxCompletionTokens;
    }
    if (t == "none" || t == "omit") {
        return MaxTokensField::None;
    }
    throw std::invalid_argument("unknown --max-tokens-field: " + token +
                                " (expected auto, max_tokens, max_completion_tokens, or none)");
}

TriState parse_tri_state(const std::string& token) {
    const std::string t = ascii_lower(trim_copy(token));
    if (t == "auto") {
        return TriState::Auto;
    }
    if (t == "send" || t == "on" || t == "true") {
        return TriState::Send;
    }
    if (t == "omit" || t == "off" || t == "false" || t == "none") {
        return TriState::Omit;
    }
    throw std::invalid_argument("invalid policy value: " + token + " (expected auto, send, or omit)");
}

std::string default_url_for_mode_token(const std::string& mode_token) {
    const std::string m = ascii_lower(trim_copy(mode_token));
    if (m == "gemini" || m == "google") {
        return "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions";
    }
    if (m == "openai" || m == "openai-reasoning") {
        return "https://api.openai.com/v1/chat/completions";
    }
    if (m == "anthropic" || m == "claude") {
        return "https://api.anthropic.com/v1/chat/completions";
    }
    if (m == "deepseek") {
        return "https://api.deepseek.com/v1/chat/completions";
    }
    if (m == "openrouter") {
        return "https://openrouter.ai/api/v1/chat/completions";
    }
    if (m == "xai" || m == "grok") {
        return "https://api.x.ai/v1/chat/completions";
    }
    if (m == "groq") {
        return "https://api.groq.com/openai/v1/chat/completions";
    }
    if (m == "mistral") {
        return "https://api.mistral.ai/v1/chat/completions";
    }
    if (m == "dashscope" || m == "qwen") {
        return "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    }
    if (m == "siliconflow") {
        return "https://api.siliconflow.cn/v1/chat/completions";
    }
    if (m == "moonshot" || m == "kimi") {
        return "https://api.moonshot.cn/v1/chat/completions";
    }
    if (m == "zhipu" || m == "glm") {
        return "https://open.bigmodel.cn/api/paas/v4/chat/completions";
    }
    if (m == "ollama") {
        return "http://127.0.0.1:11434/v1/chat/completions";
    }
    return {};
}

std::string build_vertex_chat_completions_url(const std::string& project, const std::string& location) {
    const std::string proj = trim_copy(project);
    if (proj.empty()) {
        return {};
    }
    std::string loc = ascii_lower(trim_copy(location));
    if (loc.empty()) {
        loc = "global";
    }
    if (loc == "global") {
        return "https://aiplatform.googleapis.com/v1/projects/" + proj +
               "/locations/global/endpoints/openapi/chat/completions";
    }
    return "https://" + loc + "-aiplatform.googleapis.com/v1/projects/" + proj + "/locations/" + loc +
           "/endpoints/openapi/chat/completions";
}

std::string normalize_model_for_endpoint(const std::string& model, const std::string& api_url) {
    const std::string trimmed = trim_copy(model);
    if (trimmed.empty() || trimmed.find('/') != std::string::npos) {
        return trimmed;
    }
    const std::string u = ascii_lower(api_url);
    if (u.find("aiplatform.googleapis.com") != std::string::npos &&
        u.find("/endpoints/openapi") != std::string::npos) {
        const std::string lm = ascii_lower(trimmed);
        if (starts_with(lm, "gemini-") || starts_with(lm, "gemma-")) {
            return "google/" + trimmed;
        }
        if (starts_with(lm, "claude-")) {
            return "anthropic/" + trimmed;
        }
        if (starts_with(lm, "gpt-")) {
            return "openai/" + trimmed;
        }
    }
    return trimmed;
}

VertexConfig resolve_vertex_config(const std::string& explicit_project,
                                   const std::string& explicit_location,
                                   bool fetch_token) {
    VertexConfig cfg;
    nlohmann::json adc;
    const bool have_adc = load_json_file(default_adc_path(), adc);
    if (have_adc && adc.contains("quota_project_id") && adc["quota_project_id"].is_string()) {
        cfg.quota_project = trim_copy(adc["quota_project_id"].get<std::string>());
    }

    cfg.project = trim_copy(explicit_project);
    if (cfg.project.empty()) {
        cfg.project = first_non_empty_env({
            "VERTEX_PROJECT",
            "GOOGLE_CLOUD_PROJECT",
            "CLOUD_ML_PROJECT_ID",
            "VERTEXAI_PROJECT",
            "GCLOUD_PROJECT",
            "CLOUDSDK_CORE_PROJECT",
            "ANTHROPIC_VERTEX_PROJECT_ID",
        });
    }
    if (cfg.project.empty() && have_adc) {
        if (adc.contains("project_id") && adc["project_id"].is_string()) {
            cfg.project = trim_copy(adc["project_id"].get<std::string>());
        }
        if (cfg.project.empty() && !cfg.quota_project.empty()) {
            cfg.project = cfg.quota_project;
        }
    }
    if (cfg.project.empty()) {
        cfg.project = run_cmd_capture({"gcloud", "config", "get-value", "project", "--quiet"});
    }

    cfg.location = trim_copy(explicit_location);
    if (cfg.location.empty()) {
        cfg.location = first_non_empty_env({
            "VERTEX_LOCATION",
            "GOOGLE_CLOUD_LOCATION",
            "VERTEXAI_LOCATION",
        });
    }
    if (cfg.location.empty()) {
        cfg.location = "global";
    }

    if (fetch_token) {
        cfg.access_token = first_non_empty_env({
            "PS5AI_API_KEY",
            "VERTEX_API_KEY",
            "GOOGLE_OAUTH_ACCESS_TOKEN",
            "CLOUDSDK_AUTH_ACCESS_TOKEN",
        });
        if (cfg.access_token.empty() && have_adc) {
            cfg.access_token = exchange_adc_refresh_token(adc);
        }
        if (cfg.access_token.empty()) {
            cfg.access_token = run_cmd_capture({"gcloud", "auth", "print-access-token", "--quiet"});
        }
        if (cfg.access_token.empty()) {
            cfg.access_token =
                run_cmd_capture({"gcloud", "auth", "application-default", "print-access-token", "--quiet"});
        }
        if (cfg.access_token.empty()) {
            cfg.access_token = first_non_empty_env({"GOOGLE_API_KEY", "GEMINI_API_KEY"});
        }
    }
    return cfg;
}

std::vector<std::string> api_key_env_candidates(ApiMode resolved_mode, const std::string& mode_token) {
    std::vector<std::string> keys = {"PS5AI_API_KEY"};
    const std::string m = ascii_lower(trim_copy(mode_token));
    auto add = [&](const char* k) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
            keys.emplace_back(k);
        }
    };
    if (is_vertex_mode_token(m)) {
        add("VERTEX_API_KEY");
        add("GOOGLE_OAUTH_ACCESS_TOKEN");
        add("CLOUDSDK_AUTH_ACCESS_TOKEN");
        add("GOOGLE_API_KEY");
        add("GEMINI_API_KEY");
    } else if (resolved_mode == ApiMode::Gemini || m == "gemini" || m == "google") {
        add("GEMINI_API_KEY");
        add("GOOGLE_API_KEY");
        add("GOOGLE_OAUTH_ACCESS_TOKEN");
    } else if (resolved_mode == ApiMode::Anthropic || m == "anthropic" || m == "claude") {
        add("ANTHROPIC_API_KEY");
    } else if (resolved_mode == ApiMode::DeepSeek || m == "deepseek") {
        add("DEEPSEEK_API_KEY");
    } else if (resolved_mode == ApiMode::OpenRouter || m == "openrouter") {
        add("OPENROUTER_API_KEY");
    } else if (resolved_mode == ApiMode::XAI || m == "xai" || m == "grok") {
        add("XAI_API_KEY");
    } else if (m == "groq") {
        add("GROQ_API_KEY");
    } else if (m == "mistral") {
        add("MISTRAL_API_KEY");
    } else if (m == "dashscope" || m == "qwen") {
        add("DASHSCOPE_API_KEY");
    } else if (m == "siliconflow") {
        add("SILICONFLOW_API_KEY");
    } else if (m == "moonshot" || m == "kimi") {
        add("MOONSHOT_API_KEY");
    } else if (m == "zhipu" || m == "glm") {
        add("ZHIPU_API_KEY");
    }
    add("OPENAI_API_KEY");
    return keys;
}

std::string normalize_chat_completions_url(const std::string& url) {
    const std::string trimmed = trim_copy(url);
    if (trimmed.empty()) {
        return trimmed;
    }
    const auto qpos = trimmed.find('?');
    std::string base = qpos == std::string::npos ? trimmed : trimmed.substr(0, qpos);
    const std::string query = qpos == std::string::npos ? std::string() : trimmed.substr(qpos);
    const std::string lower = ascii_lower(base);

    const char* bare_roots[] = {
        "/endpoints/openapi",
        "/v1beta/openai",
        "/compatible-mode/v1",
        "/api/paas/v4",
        "/openai/v1",
        "/api/v1",
        "/v1",
    };
    for (const char* suffix : bare_roots) {
        const std::string with_slash = std::string(suffix) + "/";
        if (ends_with(lower, suffix)) {
            return base + "/chat/completions" + query;
        }
        if (ends_with(lower, with_slash.c_str())) {
            return base + "chat/completions" + query;
        }
    }
    return trimmed;
}

ApiMode resolve_api_mode(ApiMode configured, const std::string& api_url) {
    if (configured != ApiMode::Auto) {
        return configured;
    }
    const std::string u = ascii_lower(api_url);
    if (u.find("generativelanguage.googleapis.com") != std::string::npos ||
        u.find("aiplatform.googleapis.com") != std::string::npos) {
        return ApiMode::Gemini;
    }
    if (u.find("api.openai.com") != std::string::npos || u.find(".openai.azure.com") != std::string::npos ||
        u.find(".services.ai.azure.com") != std::string::npos ||
        u.find(".cognitiveservices.azure.com") != std::string::npos) {
        return ApiMode::OpenAI;
    }
    if (u.find("api.anthropic.com") != std::string::npos) {
        return ApiMode::Anthropic;
    }
    if (u.find("api.deepseek.com") != std::string::npos) {
        return ApiMode::DeepSeek;
    }
    if (u.find("openrouter.ai") != std::string::npos) {
        return ApiMode::OpenRouter;
    }
    if (u.find("api.x.ai") != std::string::npos) {
        return ApiMode::XAI;
    }
    const char* compat_hosts[] = {
        "api.groq.com",        "api.mistral.ai",         "dashscope.aliyuncs.com",
        "api.siliconflow.cn",  "api.siliconflow.com",    "api.moonshot.cn",
        "api.moonshot.ai",     "open.bigmodel.cn",       "volces.com",
        "api.together.xyz",    "api.fireworks.ai",       "api.perplexity.ai",
        "api.cerebras.ai",     "api.sambanova.ai",       ":11434/",
    };
    for (const char* host : compat_hosts) {
        if (u.find(host) != std::string::npos) {
            return ApiMode::Compat;
        }
    }
    return ApiMode::Local;
}

ParsedChat parse_chat_response(const std::string& body) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("response is not JSON: ") + ex.what() + " body=" + snippet(body));
    }
    if (root.contains("error") && !root.contains("choices")) {
        throw std::runtime_error("API error payload: " + snippet(body));
    }
    if (root.contains("choices") && root["choices"].is_array() && root["choices"].empty() &&
        (root.contains("promptFeedback") || root.contains("prompt_filter_results"))) {
        ParsedChat blocked;
        blocked.content = "抱歉，该请求因安全策略限制无法回答。";
        blocked.tokens = 1;
        blocked.content_filtered = true;
        return blocked;
    }
    if (!root.contains("choices") || !root["choices"].is_array() || root["choices"].empty() ||
        !root["choices"][0].is_object()) {
        throw std::runtime_error("API response missing choices[0].message: " + snippet(body));
    }
    const auto& choice0 = root["choices"][0];
    std::string finish_reason;
    if (choice0.contains("finish_reason") && choice0["finish_reason"].is_string()) {
        finish_reason = ascii_lower(choice0["finish_reason"].get<std::string>());
    }
    const bool filtered_reason = finish_reason == "content_filter" || finish_reason == "safety" ||
                                 finish_reason == "prohibited_content" || finish_reason == "blocklist";

    if (!choice0.contains("message") || !choice0["message"].is_object()) {
        if (filtered_reason) {
            ParsedChat blocked;
            blocked.content = "抱歉，该请求因安全策略限制无法回答。";
            blocked.tokens = 1;
            blocked.content_filtered = true;
            return blocked;
        }
        throw std::runtime_error("API response missing choices[0].message: " + snippet(body));
    }

    const auto& msg = choice0["message"];
    std::string content;
    if (msg.contains("content") && msg["content"].is_string()) {
        content = msg["content"].get<std::string>();
    } else if (msg.contains("content") && msg["content"].is_array()) {
        for (const auto& part : msg["content"]) {
            if (part.is_string()) {
                content += part.get<std::string>();
            } else if (part.is_object()) {
                std::string ptype;
                if (part.contains("type") && part["type"].is_string()) {
                    ptype = ascii_lower(part["type"].get<std::string>());
                }
                if (ptype == "thinking" || ptype == "reasoning" || ptype == "redacted_thinking" || ptype == "thought") {
                    continue;
                }
                if (part.contains("text") && part["text"].is_string()) {
                    content += part["text"].get<std::string>();
                }
            }
        }
    } else if (msg.contains("content") && !msg["content"].is_null()) {
        content = msg["content"].dump();
    }

    if (trim_copy(content).empty() && msg.contains("refusal") && msg["refusal"].is_string()) {
        content = msg["refusal"].get<std::string>();
    }
    if (trim_copy(content).empty() && msg.contains("reasoning_content") && msg["reasoning_content"].is_string()) {
        content = msg["reasoning_content"].get<std::string>();
    }
    ParsedChat parsed;
    if (trim_copy(content).empty() && filtered_reason) {
        content = "抱歉，该请求因安全策略限制无法回答。";
        parsed.content_filtered = true;
    }
    parsed.content = content;
    if (root.contains("usage") && root["usage"].is_object()) {
        const auto& usage = root["usage"];
        if (usage.contains("completion_tokens") && usage["completion_tokens"].is_number()) {
            parsed.tokens = usage["completion_tokens"].get<long long>();
            parsed.tokens_from_usage = true;
        } else if (usage.contains("output_tokens") && usage["output_tokens"].is_number()) {
            parsed.tokens = usage["output_tokens"].get<long long>();
            parsed.tokens_from_usage = true;
        }
    }
    if (!parsed.tokens_from_usage) {
        parsed.tokens = estimate_tokens(content);
    }
    return parsed;
}

ApiMode LlmClient::effective_mode() const {
    return resolve_api_mode(api_mode, api_url);
}

bool LlmClient::omits_temperature_for_request() const {
    if (temperature_policy == TriState::Omit) {
        return true;
    }
    if (temperature_policy == TriState::Send) {
        return false;
    }
    const ApiMode mode = effective_mode();
    if (mode == ApiMode::Gemini || mode == ApiMode::OpenAIReasoning) {
        return true;
    }
    if (mode == ApiMode::OpenAI) {
        return is_openai_reasoning_series(model, false);
    }
    const std::string lower_model = ascii_lower(model);
    if (mode == ApiMode::DeepSeek) {
        return lower_model.find("reasoner") != std::string::npos;
    }
    if (mode == ApiMode::OpenRouter || mode == ApiMode::Compat) {
        if (lower_model.find("gemini-4") != std::string::npos ||
            lower_model.find("gemini-3.6") != std::string::npos ||
            lower_model.find("gemini-3.8") != std::string::npos ||
            lower_model.find("deepseek-reasoner") != std::string::npos) {
            return true;
        }
        return is_openai_reasoning_series(model, true);
    }
    return false;
}

std::string LlmClient::build_request_body(const std::vector<ChatMessage>& messages, double temperature) const {
    const ApiMode mode = effective_mode();
    nlohmann::json payload;
    payload["model"] = normalize_model_for_endpoint(model, api_url);
    payload["messages"] = nlohmann::json::array();
    for (const auto& message : messages) {
        payload["messages"].push_back({{"role", message.role}, {"content", message.content}});
    }
    if (!omits_temperature_for_request()) {
        payload["temperature"] = temperature;
    }
    payload["stream"] = false;

    MaxTokensField tf = max_tokens_field;
    if (tf == MaxTokensField::Auto) {
        tf = (mode == ApiMode::OpenAI || mode == ApiMode::OpenAIReasoning) ? MaxTokensField::MaxCompletionTokens
                                                                           : MaxTokensField::MaxTokens;
    }
    if (tf == MaxTokensField::MaxTokens && max_tokens > 0) {
        payload["max_tokens"] = max_tokens;
    } else if (tf == MaxTokensField::MaxCompletionTokens && max_tokens > 0) {
        payload["max_completion_tokens"] = max_tokens;
    }

    const bool send_thinking_off =
        thinking_off_policy == TriState::Send || (thinking_off_policy == TriState::Auto && mode == ApiMode::Local);
    if (send_thinking_off) {
        payload["chat_template_kwargs"] = {{"enable_thinking", false}};
        payload["enable_thinking"] = false;
    }

    if (!reasoning_effort.empty()) {
        payload["reasoning_effort"] = reasoning_effort;
    }

    return payload.dump();
}

LlmResponse LlmClient::complete(const std::vector<ChatMessage>& messages, double temperature) const {
    ensure_curl();
    const std::string body = build_request_body(messages, temperature);
    const int retries = std::max(0, max_retries);
    bool refreshed_once = false;

    for (int attempt = 0;; ++attempt) {
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            throw std::runtime_error("curl_easy_init failed");
        }
        struct CurlGuard {
            CURL* handle;
            ~CurlGuard() { curl_easy_cleanup(handle); }
        } guard{curl};

        std::string response;
        int retry_after_s = 0;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        const std::string& active_key = refreshed_api_key.empty() ? api_key : refreshed_api_key;
        if (!active_key.empty()) {
            const std::string auth = "Authorization: Bearer " + active_key;
            headers = curl_slist_append(headers, auth.c_str());
        }
        headers = curl_slist_append(headers, "Connection: close");
        headers = curl_slist_append(headers, "Accept: application/json");
        for (const auto& extra : extra_headers) {
            if (!extra.empty()) {
                headers = curl_slist_append(headers, extra.c_str());
            }
        }
        struct HeaderGuard {
            curl_slist* list;
            ~HeaderGuard() { curl_slist_free_all(list); }
        } header_guard{headers};

        curl_easy_setopt(curl, CURLOPT_URL, api_url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &retry_after_s);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "ps5ai-11dim/1.1");

        const auto started = std::chrono::steady_clock::now();
        const CURLcode rc = curl_easy_perform(curl);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        if (rc != CURLE_OK) {
            if (attempt < retries && is_retryable_curl(rc)) {
                const int wait_s = std::min(30, 2 * (1 << std::min(attempt, 4)));
                std::cout << "  [RETRY " << (attempt + 1) << "/" << retries << "] curl " << curl_easy_strerror(rc)
                          << ", waiting " << wait_s << "s...\n"
                          << std::flush;
                std::this_thread::sleep_for(std::chrono::seconds(wait_s));
                continue;
            }
            throw std::runtime_error(std::string("API Request failed: curl ") + curl_easy_strerror(rc) +
                                     " body=" + snippet(response));
        }
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 401 && !refreshed_once && token_refresher) {
            refreshed_once = true;
            const std::string fresh = token_refresher();
            if (!fresh.empty() && fresh != active_key) {
                refreshed_api_key = fresh;
                std::cout << "  [AUTH] OAuth access token refreshed on HTTP 401, retrying...\n" << std::flush;
                continue;
            }
        }
        if (http_code >= 400 || http_code == 0) {
            if (attempt < retries && is_retryable_http(http_code)) {
                int wait_s = std::min(30, 2 * (1 << std::min(attempt, 4)));
                if (retry_after_s > 0) {
                    wait_s = std::min(60, std::max(1, retry_after_s));
                }
                std::cout << "  [RETRY " << (attempt + 1) << "/" << retries << "] HTTP " << http_code
                          << ", waiting " << wait_s << "s...\n"
                          << std::flush;
                std::this_thread::sleep_for(std::chrono::seconds(wait_s));
                continue;
            }
            throw std::runtime_error("API Request failed: http " + std::to_string(http_code) +
                                     " body=" + snippet(response));
        }

        ParsedChat parsed = parse_chat_response(response);
        LlmResponse out;
        out.content = std::move(parsed.content);
        out.elapsed_s = elapsed;
        out.tokens = parsed.tokens;
        // Non-streaming endpoint: same TTFT approximation as the Python v3 harness.
        out.ttft_s = elapsed * 0.18;
        return out;
    }
}
