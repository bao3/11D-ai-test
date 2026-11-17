#include "http.hpp"
#include "long_context.hpp"
#include "perf.hpp"
#include "prompt_data.hpp"
#include "prompts.hpp"
#include "report.hpp"
#include "sandbox.hpp"
#include "scoring.hpp"
#include "selftest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Options {
    std::string api_url = "http://127.0.0.1:8081/v1/chat/completions";
    std::string api_key = "sk-local-test";
    std::string model;
    std::string output = "benchmark_report_v3.md";
    int max_tokens = 65536;
    int timeout_s = 2400;
    ApiMode api_mode = ApiMode::Auto;
    std::string api_mode_token = "auto";
    MaxTokensField max_tokens_field = MaxTokensField::Auto;
    TriState temperature_policy = TriState::Auto;
    TriState thinking_off_policy = TriState::Auto;
    std::string reasoning_effort;
    std::string vertex_project;
    std::string vertex_location;
    std::function<std::string()> token_refresher;
    int retries = -1;
    std::vector<std::string> extra_headers;
    bool api_url_explicit = false;
    bool api_key_explicit = false;
    bool self_test = false;
    bool help = false;
};

void print_help() {
    std::cout
        << "11dim-ai-test — 11D AI Test eleven-dimension LLM benchmark (C++17, Linux & macOS)\n"
        << "\n"
        << "Usage:\n"
        << "  11dim-ai-test --model <name> [options]\n"
        << "  11dim-ai-test --self-test\n"
        << "  11dim-ai-test --help\n"
        << "\n"
        << "Options:\n"
        << "  --api-url <url>               OpenAI-compatible chat completions endpoint\n"
        << "                                (default: http://127.0.0.1:8081/v1/chat/completions,\n"
        << "                                or provider default when --api-mode names a cloud provider)\n"
        << "  --api-key <key>               Bearer token (or env PS5AI_API_KEY / GEMINI_API_KEY /\n"
        << "                                OPENAI_API_KEY / ANTHROPIC_API_KEY / gcloud ADC on vertex)\n"
        << "  --model <name>                Model id sent in the request body (required for a live run)\n"
        << "  --api-mode <mode>             Provider compatibility preset (default: auto):\n"
        << "                                  auto             Detect from --api-url (falls back to local)\n"
        << "                                  local / ps5ai    llama-server/vLLM (sends enable_thinking=false)\n"
        << "                                  compat           Strict standard OpenAI /v1/chat/completions\n"
        << "                                  gemini           Google AI Studio Gemini (omits temperature)\n"
        << "                                  vertex           GCP Vertex AI OpenAI endpoint (auto gcloud/ADC)\n"
        << "                                  openai           OpenAI / Azure (uses max_completion_tokens)\n"
        << "                                  openai-reasoning OpenAI o-series / GPT-5 (omits temperature)\n"
        << "                                  anthropic        Anthropic OpenAI-compat endpoint\n"
        << "                                  deepseek / openrouter / xai / groq / mistral /\n"
        << "                                  dashscope / siliconflow / moonshot / zhipu / ollama\n"
        << "  --vertex-project <id>         GCP project ID for Vertex AI (or GOOGLE_CLOUD_PROJECT / gcloud)\n"
        << "  --vertex-location <loc>       GCP Vertex location (default: global)\n"
        << "  --max-tokens <n>              Token cap sent to the API (default: 65536)\n"
        << "  --max-tokens-field <field>    auto | max_tokens | max_completion_tokens | none\n"
        << "  --temperature-mode <policy>   auto | send | omit (shorthand: --omit-temperature)\n"
        << "  --thinking-off-fields <pol>   auto | send | omit (controls enable_thinking=false fields)\n"
        << "  --reasoning-effort <level>    Optional OpenAI/Gemini reasoning_effort (e.g. low|medium|high)\n"
        << "  --retries <n>                 Retries on HTTP 429/5xx (default: 0 for local, 3 for cloud)\n"
        << "  --header <K: V>               Extra HTTP header (repeatable, e.g. Azure api-key)\n"
        << "  --output <path>               Markdown report path (default: benchmark_report_v3.md)\n"
        << "                                A JSON sidecar is written next to it.\n"
        << "  --timeout <sec>               Per-request timeout (default: 2400)\n"
        << "  --self-test                   Score math/Chinese/JSON/sandbox/HTTP modes offline. No LLM.\n"
        << "  --help                        Show this help\n"
        << "\n"
        << "Dimension 3 compiles model output with -std=c++17 -pthread and runs it.\n"
        << "Compiler order: PS5AI_CXX, PS5AI_GXX, PS5AI_TOOLCHAIN, bundled toolchain, then g++/clang++/c++.\n"
        << "Those scores are not comparable to the old Python asyncio sandbox.\n";
}

std::string need_value(int& i, int argc, char** argv, const std::string& flag) {
    if (i + 1 >= argc) {
        throw UsageError("missing value for " + flag);
    }
    ++i;
    return argv[i];
}

int to_positive(const std::string& text, const std::string& flag) {
    try {
        std::size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used != text.size() || value <= 0) {
            throw UsageError(flag + " must be a positive integer");
        }
        return value;
    } catch (const UsageError&) {
        throw;
    } catch (const std::exception&) {
        throw UsageError(flag + " must be a positive integer");
    }
}

int to_non_negative(const std::string& text, const std::string& flag) {
    try {
        std::size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used != text.size() || value < 0) {
            throw UsageError(flag + " must be a non-negative integer");
        }
        return value;
    } catch (const UsageError&) {
        throw;
    } catch (const std::exception&) {
        throw UsageError(flag + " must be a non-negative integer");
    }
}

bool match_flag(const std::string& arg, const std::string& flag) {
    if (arg == flag) {
        return true;
    }
    const std::string prefix = flag + "=";
    return arg.compare(0, prefix.size(), prefix) == 0;
}

Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take = [&](const std::string& flag) {
            const std::string prefix = flag + "=";
            if (arg == flag) {
                return need_value(i, argc, argv, flag);
            }
            return arg.substr(prefix.size());
        };
        if (arg == "--help" || arg == "-h") {
            opt.help = true;
        } else if (arg == "--self-test") {
            opt.self_test = true;
        } else if (arg == "--omit-temperature") {
            opt.temperature_policy = TriState::Omit;
        } else if (match_flag(arg, "--api-url")) {
            opt.api_url = take("--api-url");
            opt.api_url_explicit = true;
        } else if (match_flag(arg, "--api-key")) {
            opt.api_key = take("--api-key");
            opt.api_key_explicit = true;
        } else if (match_flag(arg, "--model")) {
            opt.model = take("--model");
        } else if (match_flag(arg, "--output")) {
            opt.output = take("--output");
        } else if (match_flag(arg, "--api-mode")) {
            const std::string token = take("--api-mode");
            try {
                opt.api_mode = parse_api_mode(token);
                opt.api_mode_token = token;
            } catch (const std::exception& ex) {
                throw UsageError(ex.what());
            }
        } else if (match_flag(arg, "--max-tokens-field")) {
            try {
                opt.max_tokens_field = parse_max_tokens_field(take("--max-tokens-field"));
            } catch (const std::exception& ex) {
                throw UsageError(ex.what());
            }
        } else if (match_flag(arg, "--temperature-mode")) {
            try {
                opt.temperature_policy = parse_tri_state(take("--temperature-mode"));
            } catch (const std::exception& ex) {
                throw UsageError(ex.what());
            }
        } else if (match_flag(arg, "--thinking-off-fields")) {
            try {
                opt.thinking_off_policy = parse_tri_state(take("--thinking-off-fields"));
            } catch (const std::exception& ex) {
                throw UsageError(ex.what());
            }
        } else if (match_flag(arg, "--reasoning-effort")) {
            opt.reasoning_effort = take("--reasoning-effort");
        } else if (match_flag(arg, "--vertex-project")) {
            opt.vertex_project = take("--vertex-project");
        } else if (match_flag(arg, "--vertex-location")) {
            opt.vertex_location = take("--vertex-location");
        } else if (match_flag(arg, "--retries")) {
            opt.retries = to_non_negative(take("--retries"), "--retries");
        } else if (match_flag(arg, "--header")) {
            opt.extra_headers.push_back(take("--header"));
        } else if (match_flag(arg, "--max-tokens")) {
            opt.max_tokens = to_positive(take("--max-tokens"), "--max-tokens");
        } else if (match_flag(arg, "--timeout")) {
            opt.timeout_s = to_positive(take("--timeout"), "--timeout");
        } else {
            throw UsageError("unknown argument: " + arg);
        }
    }
    return opt;
}

std::string now_stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm local {};
    localtime_r(&t, &local);
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local) == 0) {
        return {};
    }
    return buf;
}

void print_details(const ScoreResult& result) {
    for (const auto& line : result.details) {
        std::cout << "  " << line << "\n";
    }
    std::cout << "  score=" << std::fixed << std::setprecision(1) << result.score << std::endl;
}

struct Agg {
    double time_s = 0;
    long long tokens = 0;
    double ttft_sum = 0;
    int n = 0;
    std::chrono::steady_clock::time_point wall_start{};
    bool wall = false;

    void begin() {
        wall_start = std::chrono::steady_clock::now();
        wall = true;
    }
    void add(const LlmResponse& response) {
        time_s += response.elapsed_s;
        tokens += response.tokens;
        ttft_sum += response.ttft_s;
        ++n;
    }
    PerfSample sample() const {
        PerfSample perf;
        if (wall) {
            perf.time_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
        } else {
            perf.time_s = time_s;
        }
        perf.tokens = tokens;
        perf.ttft_s = n > 0 ? ttft_sum / n : 0;
        return perf;
    }
};

int run_benchmark(const Options& opt) {
    LlmClient client;
    client.api_url = opt.api_url;
    client.api_key = opt.api_key;
    client.model = opt.model;
    client.max_tokens = opt.max_tokens;
    client.timeout_s = opt.timeout_s;
    client.api_mode = opt.api_mode;
    client.max_tokens_field = opt.max_tokens_field;
    client.temperature_policy = opt.temperature_policy;
    client.thinking_off_policy = opt.thinking_off_policy;
    client.reasoning_effort = opt.reasoning_effort;
    client.extra_headers = opt.extra_headers;
    client.token_refresher = opt.token_refresher;

    const ApiMode eff_mode = client.effective_mode();
    client.max_retries = opt.retries >= 0 ? opt.retries : (eff_mode == ApiMode::Local ? 0 : 3);
    const bool temp_omitted = client.omits_temperature_for_request();
    const std::string wire_model = normalize_model_for_endpoint(opt.model, client.api_url);

    const std::string cxx = resolve_cxx_compiler();
    const std::string cxx_ver = cxx_version_line(cxx);
    std::cout << std::string(78, '=') << "\n";
    std::cout << "11D AI Test 十一维评测 | 模型: " << opt.model;
    if (wire_model != opt.model) {
        std::cout << " (请求 ID: " << wire_model << ")";
    }
    std::cout << "\n";
    std::cout << "接口地址: " << client.api_url << "\n";
    std::cout << "接口模式: " << api_mode_name(eff_mode)
              << (temp_omitted ? " (已省略 temperature)" : "")
              << " | 限流重试: " << client.max_retries << "\n";
    std::cout << "维度 3 编译器: " << cxx;
    if (!cxx_ver.empty()) {
        std::cout << " (" << cxx_ver << ")";
    }
    std::cout << "\n";
    std::cout << "维度 3 分数不可与旧 Python asyncio 沙盒对比（-std=c++17 -pthread）\n";
    std::cout << "维度 10 与维度 4 共用同一次伦理光谱请求结果（不重复打接口）\n";
    std::cout << std::string(78, '=') << std::endl;

    std::vector<std::pair<std::string, double>> scores;
    std::vector<std::pair<std::string, PerfSample>> perf;
    auto single = [&](const std::string& prompt, double temperature) {
        return client.complete({{"user", prompt}}, temperature);
    };
    bool had_env_error = false;
    std::vector<std::string> env_error_details;
    auto note_env = [&](const std::string& detail) {
        had_env_error = true;
        env_error_details.push_back(detail);
    };
    auto clamp100 = [](double v) { return std::min(100.0, std::max(0.0, v)); };
    auto fail_dim = [&](const std::string& name, const std::string& dim_key, const std::string& err) {
        std::cout << "  [ERROR] " << err << "\n";
        std::cout << "  score=0.0 (维度失败，继续后续维度)\n";
        scores.emplace_back(name, 0.0);
        perf.emplace_back(dim_key, PerfSample{});
    };

    double ethics_score_raw = 0;
    double ethics_score = 0;
    std::string ethics_summary_str;

    // 1 Chinese
    std::cout << "[1/11] 正在执行：中文理解测试..." << std::endl;
    try {
        LlmResponse r = single(kPromptChinese(), 0.2);
        ScoreResult sc = evaluate_chinese(r.content);
        print_details(sc);
        scores.emplace_back("1. 中文语境理解", clamp100(sc.score));
        perf.emplace_back("1", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("1. 中文语境理解", "1", ex.what());
    }

    // 2 Math
    std::cout << "[2/11] 正在执行：7步数学推理测试..." << std::endl;
    try {
        LlmResponse r = single(kPromptMath(), 0.0);
        ScoreResult sc = evaluate_math(r.content);
        print_details(sc);
        scores.emplace_back("2. 7步数学推理", clamp100(sc.score));
        perf.emplace_back("2", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("2. 7步数学推理", "2", ex.what());
    }

    // 3 Code sandbox
    std::cout << "[3/11] 正在执行：C++17 代码沙盒实测..." << std::endl;
    try {
        LlmResponse r = single(prompt_code(), 0.1);
        ScoreResult sc = evaluate_code(r.content);
        print_details(sc);
        for (const auto& line : sc.details) {
            if (line.find("[ENV_ERROR]") != std::string::npos) {
                std::cout << "  !! 环境问题（非模型能力）：见上 details\n";
                note_env(line);
            }
        }
        scores.emplace_back("3. 异步并发代码实测", clamp100(sc.score));
        perf.emplace_back("3", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("3. 异步并发代码实测", "3", ex.what());
    }

    // 4 Ethics
    std::cout << "[4/11] 正在执行：伦理光谱与被拒率测试..." << std::endl;
    try {
        Agg ethics;
        ethics.begin();
        ethics_score_raw = 0;
        int refused = 0;
        for (int i = 0; i < kEthicsCaseCount; ++i) {
            const EthicsCase& item = kEthicsCases[i];
            LlmResponse er = single(item.prompt, 0.2);
            ethics.add(er);
            const bool is_refused = ethics_refused(er.content);
            if (is_refused) {
                ++refused;
            }
            const double points = ethics_points(item.expected, is_refused);
            ethics_score_raw += points;
            std::cout << "  " << item.id << " " << item.name << " expected=" << item.expected
                      << (is_refused ? " refused" : " answered") << " +" << points << "\n";
        }
        const PerfSample ethics_perf = ethics.sample();
        std::ostringstream ethics_summary;
        ethics_summary << std::fixed << std::setprecision(1);
        ethics_summary << "总拒答率: " << refused << "/" << kEthicsCaseCount << " ("
                       << (100.0 * refused / kEthicsCaseCount) << "%)";
        ethics_summary_str = ethics_summary.str();
        ethics_score = clamp100(ethics_score_raw);
        std::cout << "  " << ethics_summary_str << "\n";
        if (ethics_score_raw > 100.0) {
            std::cout << "  raw_score=" << std::fixed << std::setprecision(1) << ethics_score_raw
                      << " -> clamped=" << ethics_score << std::endl;
        } else {
            std::cout << "  score=" << std::fixed << std::setprecision(1) << ethics_score << std::endl;
        }
        scores.emplace_back("4. 伦理安全与边界", ethics_score);
        perf.emplace_back("4", ethics_perf);
    } catch (const std::exception& ex) {
        ethics_score = 0;
        ethics_score_raw = 0;
        ethics_summary_str = std::string("伦理维失败: ") + ex.what();
        fail_dim("4. 伦理安全与边界", "4", ex.what());
    }

    // 5 JSON
    std::cout << "[5/11] 正在执行：JSON 与 Tool Calling 测试..." << std::endl;
    try {
        LlmResponse r = single(kPromptJsonTool(), 0.0);
        ScoreResult sc = evaluate_json_tool(r.content);
        print_details(sc);
        scores.emplace_back("5. JSON 与 Tool Calling", clamp100(sc.score));
        perf.emplace_back("5", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("5. JSON 与 Tool Calling", "5", ex.what());
    }

    // 6 Long context
    std::cout << "[6/11] 正在执行：长上下文大海捞针测试..." << std::endl;
    try {
        const std::string long_prompt = build_long_context_prompt(12000);
        LlmResponse r = single(long_prompt, 0.0);
        ScoreResult sc = evaluate_long_context(r.content);
        print_details(sc);
        scores.emplace_back("6. 长上下文抗干扰", clamp100(sc.score));
        perf.emplace_back("6", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("6. 长上下文抗干扰", "6", ex.what());
    }

    // 7 Multiturn
    std::cout << "[7/11] 正在执行：多轮指令随动与抗遗忘测试..." << std::endl;
    try {
        Agg multi;
        multi.begin();
        std::vector<ChatMessage> messages{{"user", kPromptMultiturn1()}};
        LlmResponse first = client.complete(messages, 0.0);
        multi.add(first);
        messages.push_back({"assistant", first.content});
        messages.push_back({"user", kPromptMultiturn2()});
        LlmResponse second = client.complete(messages, 0.0);
        multi.add(second);
        ScoreResult sc = evaluate_multiturn_text(second.content);
        print_details(sc);
        scores.emplace_back("7. 多轮指令随动抗遗忘", clamp100(sc.score));
        perf.emplace_back("7", multi.sample());
    } catch (const std::exception& ex) {
        fail_dim("7. 多轮指令随动抗遗忘", "7", ex.what());
    }

    // 8 Hallucination
    std::cout << "[8/11] 正在执行：幻觉与事实锚定防御测试..." << std::endl;
    try {
        LlmResponse r = single(kPromptHallucination(), 0.0);
        ScoreResult sc = evaluate_hallucination_text(r.content);
        print_details(sc);
        scores.emplace_back("8. 幻觉与事实锚定", clamp100(sc.score));
        perf.emplace_back("8", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("8. 幻觉与事实锚定", "8", ex.what());
    }

    // 9 Terminology
    std::cout << "[9/11] 正在执行：中英混排与专业术语保真测试..." << std::endl;
    try {
        LlmResponse r = single(kPromptTerminology(), 0.0);
        ScoreResult sc = evaluate_terminology_text(r.content);
        print_details(sc);
        scores.emplace_back("9. 中英混排术语保真", clamp100(sc.score));
        perf.emplace_back("9", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("9. 中英混排术语保真", "9", ex.what());
    }

    // 10 shares dim 4
    std::cout << "[10/11] 细粒度渗透灰区边界：沿用维度 4 的伦理光谱得分（含 E6 授权扫描；不重复请求）" << std::endl;
    scores.emplace_back("10. 细粒度渗透灰区边界", ethics_score);
    std::cout << "  score=" << std::fixed << std::setprecision(1) << ethics_score
              << " (与维度 4 同次结果" << (ethics_score_raw > 100.0 ? "，已封顶 100" : "") << ")" << std::endl;

    // 11 Stability
    std::cout << "[11/11] 正在执行：确定性稳定性方差测试 (@ Temp 0, 3次)..." << std::endl;
    double variance = 0;
    try {
        Agg stable;
        stable.begin();
        std::vector<double> math_scores;
        for (int i = 0; i < 3; ++i) {
            LlmResponse mr = single(kPromptMath(), 0.0);
            stable.add(mr);
            const double math_score = evaluate_math(mr.content).score;
            math_scores.push_back(math_score);
            std::cout << "  run " << (i + 1) << " math=" << std::fixed << std::setprecision(1) << math_score << "\n";
        }
        variance = sample_variance(math_scores);
        const double stability = clamp100(stability_from_variance(variance));
        std::cout << "  3次运行得分记录: [" << math_scores[0] << ", " << math_scores[1] << ", " << math_scores[2] << "]\n";
        std::cout << "  分数方差 (Variance): " << std::fixed << std::setprecision(2) << variance << "\n";
        std::cout << "  score=" << std::fixed << std::setprecision(1) << stability << std::endl;
        scores.emplace_back("11. 确定性稳定性方差", stability);
        perf.emplace_back("11", stable.sample());
    } catch (const std::exception& ex) {
        fail_dim("11. 确定性稳定性方差", "11", ex.what());
    }

    ReportInput input;
    input.model = opt.model;
    input.api_url = client.api_url;
    input.timestamp = now_stamp();
    input.ethics_summary = ethics_summary_str;
    input.scores = scores;
    input.perf = perf;
    input.stability_variance = variance;
    input.peak_rss_mb = peak_rss_mb();
    input.hw_backend = detect_hardware_acceleration();
    input.env_error = had_env_error;
    input.env_error_details = env_error_details;
    input.dim3_compiler = cxx;
    input.dim3_compiler_version = cxx_ver;
    input.api_mode = api_mode_name(eff_mode);
    input.temperature_omitted = temp_omitted;
    const Report report = build_report(input);
    write_report_files(opt.output, report);

    std::cout << "\n十一维评测完成。综合得分: " << std::fixed << std::setprecision(1) << report.overall << "/100\n";
    std::cout << "Markdown: " << opt.output << "\n";
    std::cout << "JSON: " << json_sidecar_path(opt.output) << std::endl;
    if (had_env_error) {
        std::cerr << "ENV_ERROR: 评测环境异常，退出码 3（报告已写出，勿把相关维当成模型能力）\n";
        return 3;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options opt = parse_args(argc, argv);
        if (opt.help) {
            print_help();
            return 0;
        }
        if (opt.self_test) {
            return run_self_test();
        }
        if (opt.model.empty()) {
            std::cerr << "error: --model is required\n\n";
            print_help();
            return 2;
        }

        const bool want_vertex_url =
            !opt.api_url_explicit && (is_vertex_mode_token(opt.api_mode_token) || !opt.vertex_project.empty());
        if (!opt.api_url_explicit && !want_vertex_url) {
            const std::string preset_url = default_url_for_mode_token(opt.api_mode_token);
            if (!preset_url.empty()) {
                opt.api_url = preset_url;
            }
        }
        opt.api_url = normalize_chat_completions_url(opt.api_url);

        const bool target_is_vertex =
            want_vertex_url || is_vertex_mode_token(opt.api_mode_token) ||
            opt.api_url.find("aiplatform.googleapis.com") != std::string::npos;

        if (target_is_vertex) {
            const VertexConfig vcfg =
                resolve_vertex_config(opt.vertex_project, opt.vertex_location, !opt.api_key_explicit);
            if (want_vertex_url) {
                if (vcfg.project.empty()) {
                    throw UsageError(
                        "GCP Vertex project ID not found; pass --vertex-project <PROJECT_ID>, "
                        "set GOOGLE_CLOUD_PROJECT, or run 'gcloud config set project <PROJECT_ID>'");
                }
                opt.api_url = build_vertex_chat_completions_url(vcfg.project, vcfg.location);
            }
            if (!opt.api_key_explicit) {
                if (!vcfg.access_token.empty()) {
                    opt.api_key = vcfg.access_token;
                }
                const std::string proj = opt.vertex_project;
                const std::string loc = opt.vertex_location;
                opt.token_refresher = [proj, loc]() {
                    return resolve_vertex_config(proj, loc, true).access_token;
                };
            }
        } else if (!opt.api_key_explicit) {
            const ApiMode resolved_mode = resolve_api_mode(opt.api_mode, opt.api_url);
            // Prefer explicit --api-key; otherwise check PS5AI_API_KEY, provider-specific
            // env var (e.g. GEMINI_API_KEY / ANTHROPIC_API_KEY), and OPENAI_API_KEY.
            for (const auto& key_name : api_key_env_candidates(resolved_mode, opt.api_mode_token)) {
                if (const char* env = std::getenv(key_name.c_str())) {
                    if (env[0] != '\0') {
                        opt.api_key = env;
                        break;
                    }
                }
            }
        }

        return run_benchmark(opt);
    } catch (const UsageError& ex) {
        std::cerr << "error: " << ex.what() << "\n\n";
        print_help();
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
