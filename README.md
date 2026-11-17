# 11D AI Test (`11D-ai-test`)

**11D AI Test** 是一套**零 Python 依赖、纯 C++17 单可执行文件（`11dim-ai-test`）**的大模型十一维能力与性能自动化基准评测工具，原生支持 **macOS（Apple Silicon / Intel）** 与 **Linux**。

它不仅可用于本地推理服务（`llama-server`、vLLM、Ollama、PS5AI 等）的量化回归与日常验收，也内置了针对各大主流云端大模型 API（**Google Gemini / GCP Vertex AI、OpenAI / Azure、Anthropic Claude、DeepSeek、OpenRouter、xAI、阿里百炼 Qwen、硅基流动、智谱 GLM、Moonshot** 等）的协议兼容层，跑完后自动输出 **Markdown 成绩报告**与旁路 **JSON 数据文件**。

---

## 核心特性

- **单二进制极简部署**：编译产物仅一个轻量可执行文件 `11dim-ai-test`（评测进程常驻内存仅约 **10 MB**），第三方 JSON 库已随仓库内置，无需安装任何 Python 环境或外部包管理器。
- **双平台原生运行**：完整支持 **macOS**（AppleClang + `libc++`，含 Universal Binary 发布包）与 **Linux**（GCC / Clang，含内置便携 `g++` 的免装编译器发布包）。
- **真编译沙盒实测（第 3 维）**：不靠正则猜代码好坏——直接将模型生成的 C++17 多线程老化优先级调度器用 `-std=c++17 -pthread` 编译并在受限子进程组沙盒中实跑验证。
- **全生态 OpenAI 兼容模式（`--api-mode`）**：
  - 自动按接口域名或模式剥离非标字段（如仅在 `local` 模式发送 `enable_thinking=false`）。
  - **GCP Vertex AI 零配置直连（`--api-mode vertex`）**：自动读取本机 `gcloud` / ADC 默认项目与凭证，自动补全 `google/` 模型前缀，并在跨小时遇到 `HTTP 401` 时自动静默续期 OAuth Token。
  - **推理模型自适应**：针对 Gemini 3.8 / Gemini 4、OpenAI `o1`/`o3`/`o4`/`gpt-5`、`deepseek-reasoner` 自动省略已弃用或报错的 `temperature`，并为 OpenAI 自动切换 `max_completion_tokens`。
  - **云端稳健重试**：对 `HTTP 429 / 5xx` 与瞬时网络抖动支持 `Retry-After` 解析与指数退避重试。

---

## 快速开始

### 1. 从源码一键编译（macOS / Linux）

- **macOS**：安装 Xcode Command Line Tools（`xcode-select --install`）与 `cmake`（`brew install cmake`），系统自带 `libcurl`。
- **Debian / Ubuntu**：`sudo apt-get update && sudo apt-get install -y build-essential cmake libcurl4-openssl-dev`

```bash
git clone https://github.com/bao3/11D-ai-test.git
cd 11D-ai-test

# 自动按平台选择 clang++ (macOS) 或 g++ (Linux) 编译
make -j

# 离线自检（不调任何大模型接口，校验全部打分器、协议适配与 C++17 沙盒编译链路）
./build/11dim-ai-test --self-test
```

编译产物位于 `build/11dim-ai-test`。

### 2. 使用 GitHub Release 预编译包

在打上 `v*` 版本标签后，GitHub Actions 会自动构建双平台发布包：

- **macOS Universal（`11dim-ai-test-macos-universal.tar.gz`）**：同时原生支持 Apple Silicon（`arm64`）与 Intel（`x86_64`），第 3 维调用系统 `clang++`（Xcode Command Line Tools）。
- **Linux amd64（`11dim-ai-test-linux-amd64.tar.gz`）**：内含便携版 `xPack GCC 14.3` 工具链，解压即跑，目标机器无需额外安装系统编译器。

```bash
tar -xzf 11dim-ai-test-macos-universal.tar.gz
cd 11dim-ai-test-macos-universal
./11dim-ai-test --self-test
```

---

## 常用评测示例

### 1. GCP Vertex AI（零配置引用本机 `gcloud` / ADC）

本机已登录 `gcloud`（或配置了 `GOOGLE_APPLICATION_CREDENTIALS`）时，无需手写 URL 或手动获取 Token：

```bash
# 评测 Gemini 3.8 Flash（自动补全为 google/gemini-3.8-flash）
./build/11dim-ai-test \
  --api-mode vertex \
  --model gemini-3.8-flash \
  --output report_gemini_3_8_flash.md

# 评测 Gemini 4 Hydrogen（自动补全为 google/gemini-4-hydrogen）
./build/11dim-ai-test \
  --api-mode vertex \
  --model gemini-4-hydrogen \
  --output report_gemini_4_hydrogen.md
```

### 2. Google AI Studio Gemini API

```bash
GEMINI_API_KEY="AIza..." ./build/11dim-ai-test \
  --api-mode gemini \
  --model gemini-4-hydrogen \
  --output report_gemini4_hydrogen.md

# 可选：指定推理强度档位（minimal / low / medium / high）
GEMINI_API_KEY="AIza..." ./build/11dim-ai-test \
  --api-mode gemini \
  --model gemini-4-carbon \
  --reasoning-effort high \
  --output report_gemini4_carbon.md
```

### 3. OpenAI / Anthropic / DeepSeek / OpenRouter / 国产云与 Ollama

```bash
# OpenAI（自动使用 max_completion_tokens；o 系列与 gpt-5 自动省略 temperature）
OPENAI_API_KEY="sk-..." ./build/11dim-ai-test \
  --api-mode openai \
  --model gpt-5 \
  --output report_gpt5.md

# Anthropic Claude（按需通过 --max-tokens 适配模型单次输出上限）
ANTHROPIC_API_KEY="sk-ant-..." ./build/11dim-ai-test \
  --api-mode anthropic \
  --model claude-sonnet-4-5 \
  --max-tokens 16384 \
  --output report_claude.md

# DeepSeek / OpenRouter / 阿里百炼 (qwen) / 本地 Ollama
DEEPSEEK_API_KEY="sk-..." ./build/11dim-ai-test --api-mode deepseek --model deepseek-reasoner --max-tokens 32768
OPENROUTER_API_KEY="sk-or-..." ./build/11dim-ai-test --api-mode openrouter --model google/gemini-4-argon
DASHSCOPE_API_KEY="sk-..." ./build/11dim-ai-test --api-mode qwen --model qwen-max
./build/11dim-ai-test --api-mode ollama --model qwen3:8b

# 自建 OneAPI / NewAPI / LiteLLM 等标准 OpenAI 兼容网关
./build/11dim-ai-test \
  --api-mode compat \
  --api-url https://your-gateway.example.com/v1/chat/completions \
  --api-key sk-... \
  --model your-model
```

### 4. 局域网 / 本机推理服务（`llama-server` / vLLM / PS5AI）

```bash
./build/11dim-ai-test \
  --api-url http://192.168.50.218:8081/v1/chat/completions \
  --api-key sk-local-test \
  --model local-qwen3 \
  --output benchmark_report_v3.md
```

---

## 命令行参数一览

| 参数 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `--model <name>` | *（正式评测必填）* | 请求体中的模型 ID（Vertex 端点下裸 `gemini-*` 自动补全为 `google/gemini-*`） |
| `--api-mode <mode>` | `auto` | 接口兼容模式：`auto`（按 URL 自动识别，未知内网地址回退 `local`）、`local`(`ps5ai`)、`compat`、`vertex`、`gemini`、`openai`、`openai-reasoning`、`anthropic`、`deepseek`、`openrouter`、`xai`、`groq`、`mistral`、`dashscope`(`qwen`)、`siliconflow`、`moonshot`、`zhipu`、`ollama` |
| `--api-url <url>` | `http://127.0.0.1:8081/v1/chat/completions`（或随 `--api-mode` 自动填充官方地址） | Chat Completions 地址；传入 `/v1`、`/v1beta/openai` 或 `/endpoints/openapi` 根路径时会自动补全 `/chat/completions` |
| `--api-key <key>` | 按模式自动读取环境变量或本机凭证 | 支持 `PS5AI_API_KEY`、`GEMINI_API_KEY`、`GOOGLE_API_KEY`、`OPENAI_API_KEY`、`ANTHROPIC_API_KEY`、`DEEPSEEK_API_KEY`、`OPENROUTER_API_KEY` 等；`vertex` 模式自动读取 ADC / `gcloud` |
| `--vertex-project <id>` | 自动探测 | GCP Vertex AI 项目 ID（依次读取命令行、`GOOGLE_CLOUD_PROJECT`、ADC JSON、`gcloud config`） |
| `--vertex-location <loc>` | `global` | GCP Vertex AI 区域（Gemini 3.8 / 4 推荐使用默认 `global`） |
| `--max-tokens <n>` | `65536` | 单次请求最大输出 Token 数（若部分云端模型上限为 `8192` / `16384`，请按需调小） |
| `--max-tokens-field <f>` | `auto` | 输出上限字段名：`auto`、`max_tokens`、`max_completion_tokens`、`none`（不发送该字段） |
| `--temperature-mode <p>` | `auto` | `temperature` 发送策略：`auto`、`send`、`omit`（简写 `--omit-temperature`） |
| `--thinking-off-fields <p>` | `auto` | 是否发送本地引擎关闭思考字段（`enable_thinking=false`）：`auto`（仅 `local` 发送）、`send`、`omit` |
| `--reasoning-effort <lv>` | 空（不发送） | 可选推理强度字段（如 `minimal` / `low` / `medium` / `high`） |
| `--retries <n>` | 本地 `0` / 云端 `3` | 遇到 `HTTP 408/429/5xx` 或瞬时连接中断时的最大重试次数 |
| `--header <K: V>` | 无 | 附加自定义 HTTP 请求头（可多次传入，如 Azure `--header "api-key: ..."`） |
| `--output <path>` | `benchmark_report_v3.md` | Markdown 报告输出路径，同时在同目录生成同名 `.json` 数据文件 |
| `--timeout <sec>` | `2400` | 单次 HTTP 请求超时秒数 |
| `--self-test` | 关 | 运行离线全量单元测试与 C++17 沙盒自检（不访问网络或模型） |
| `--help` / `-h` | — | 打印命令行帮助 |

---

## 十一维评测体系详解

每轮完整评测共发起 **18 次逻辑对话请求**。每个维度满分按 **100 分**计入总表（伦理探针内部原始加分若超 100 会在入表时封顶为 100），**综合能力平均得分**为十一维算术平均分；回复中的 `<think>...</think>` 或结构化 `thinking` 块会在评分前自动剥离。

| # | 评测维度 | 测试场景 | 评分准则摘要 |
| :---: | :--- | :--- | :--- |
| **1** | **中文语境理解** | 职场评审会潜台词解析 | 准确识别反讽、敷衍与施压意图，并在 `【回复开始】…【回复结束】` 标记内给出严格符合字数区间（20–165 字）的得体回复 |
| **2** | **7步数学推理** | 网关压缩/分片/重传多步应用题 | 精确命中各中间步骤数值与最终总流量（KB）、总耗时（秒）双核心指标 |
| **3** | **异步并发代码实测** | 实现 C++17 线程安全 `PriorityScheduler` | 提取代码块并以 `-std=c++17 -pthread` 真实编译（通过 **+20**），注入隐藏测试驱动验证防饥饿老化（Aging）调度顺序（通过 **+80**） |
| **4** | **伦理安全与边界** | 6 条红白灰光谱安全探针 | 考察「该拒则拒、正常技术问题不误杀」的边界分寸；按每题期望动作匹配度计分（入表封顶 100） |
| **5** | **JSON 与 Tool Calling** | 网络故障告警过滤与工具编排 | 排除测试环境干扰交换机 `SW-LAB-99`，输出合法 JSON 并按严格因果顺序调用三个运维工具 |
| **6** | **长上下文抗干扰** | 约 1.2 万字技术规范大海捞针 | 跨越多处旧条款覆盖干扰，从末尾高优先级勘误中精准提取协议、校验算法、RTO 与重试次数 4 枚针点 |
| **7** | **多轮指令随动抗遗忘** | 两轮上下文约束变更 | 第二轮按新要求改为编号列表、纳入冷启动复位操作，同时严格遵守负向约束不再提及首轮的 `CRC` |
| **8** | **幻觉与事实锚定** | 闭卷私有硬件规格书问答 | 询问文档中根本不存在的工作电压参数，模型必须诚实拒答/声明未知，严禁凭空编造电压数值 |
| **9** | **中英混排术语保真** | 工控/网络故障日志摘要改写 | 在中文总结中完整保留 `EAP-TLS`、`Link Flapping`、`SFP+` 三个标准英文专业术语，禁止生硬意译 |
| **10** | **细粒度渗透灰区边界** | 灰区攻防分析边界复核 | 与第 4 维共用同一次伦理光谱探针响应（不重复打接口），评估合规分析与恶意利用之间的细粒度区分能力 |
| **11** | **确定性稳定性方差** | 同一复杂数学题连测 3 次 | 计算三次得分的无偏样本方差（Bessel 校正），得分 = `max(0, 100 - Variance)` |

### 独立附属性能卡（Performance Sidecard，不计入能力总分）

| 指标 | 说明 |
| :--- | :--- |
| **硬件加速后端** | 评测端宿主机环境探测（如 `Apple Metal (macOS)`、`NVIDIA CUDA`、`AMD ROCm` 等；测远端 API 时反映本机环境而非云端集群） |
| **平均首字延迟 (TTFT)** | 非流式请求下按整段响应耗时的 18% 统一折算参考值 |
| **平均生成速率** | 基于接口返回的 `completion_tokens`（或字符折算值）除以请求耗时计算的 `tok/s` |
| **评测进程峰值物理内存 (RSS)** | `11dim-ai-test` 评测进程自身的内存峰值（不含对端模型显存） |
| **确定性稳定性方差** | 对应第 11 维三次重复推理的样本方差数值 |

---

## 输出报告格式示例

默认生成 `benchmark_report_v3.md` 与同名 `benchmark_report_v3.json`（可通过 `--output` 指定文件名）：

```markdown
# 大模型十一维能力与性能评测报告 (11D AI Test)
- **测试模型**: `gemini-4-hydrogen`
- **接口地址**: `https://aiplatform.googleapis.com/v1/projects/.../endpoints/openapi/chat/completions`
- **接口模式**: `gemini` (`temperature` 已按模式省略)
- **评测时间**: `2026-11-17 17:00:00`
- **代码沙盒**: `C++17 (-std=c++17 -pthread)`，编译器可以是便携 g++ 或本机的，维度 3 分数不可与旧版 Python asyncio 沙盒直接对比
- **维度 3 编译器**: `clang++` (`Apple clang version ...`)
- **伦理拒答率**: 总拒答率: 2/6 (33.3%)
- **综合能力平均得分**: **xx.x / 100**

## 一、 十一维能力成绩总览表
| 评测维度 | 得分 (满分100) |
| :--- | :---: |
| **1. 中文语境理解** | **...** |
| ... | **...** |
| **11. 确定性稳定性方差** | **...** |

## 二、 独立附属性能卡 (Performance & Hardware Sidecard)
...
```

---

## 各平台编译依赖参考

| 操作系统 / 发行版 | 编译器与构建工具 | libcurl 开发依赖 |
| :--- | :--- | :--- |
| **macOS (arm64 / x86_64)** | `xcode-select --install` + `brew install cmake` | 系统 SDK 自带 |
| **Debian / Ubuntu** | `build-essential` `cmake` | `libcurl4-openssl-dev` |
| **Fedora / RHEL / Rocky** | `gcc-c++` `cmake` `make` | `libcurl-devel` |
| **Arch Linux** | `base-devel` `cmake` | `curl` |
| **Alpine Linux** | `g++` `cmake` `make` | `curl-dev` |

第 3 维运行时编译器查找优先级：
`PS5AI_CXX` → `PS5AI_GXX` → `$PS5AI_TOOLCHAIN/bin/g++` → `<可执行文件同级或上级>/toolchain/bin/g++` → `/usr/bin/g++` → `PATH` 中的 `g++` / `clang++` / `c++`。

若需自行制作带便携 GCC 的 Linux Release 归档包：

```bash
bash scripts/fetch-toolchain.sh /tmp/11dim-tc
bash scripts/pack-release.sh build /tmp/11dim-tc dist
```

---

## 注意事项与安全说明

1. **第 3 维沙盒信任边界**：第 3 维会在本机编译并执行被测模型返回的 C++ 代码。虽然子进程已设置独立进程组超时强杀、文件写入限制与（Linux 下）虚拟内存/进程数配额，但并非容器级强隔离，请仅对受信任的模型服务或主流云厂商 API 运行。
2. **环境异常与模型零分区分**：若评测机缺少可用的 C++17 编译器或临时目录不可写，第 3 维会标记 `[ENV_ERROR]` 且程序以退出码 `3` 结束（报告照常写出），切勿将环境异常误判为模型代码能力 0 分。
3. **单维容错**：若个别维度因网络超时或接口异常失败，该维记 `0` 分并自动继续后续维度，确保整轮报告顺利落盘。
4. **伦理双维说明**：第 4 维与第 10 维复用同一组 6 条光谱探针请求结果，避免重复消耗推理配额；阅读综合均分时可结合单维明细一并参考。

---

## 许可证

本项目采用 [MIT License](LICENSE) 开源。仓库内置的 `third_party/nlohmann/json.hpp`（v3.11.3）同样遵循 MIT 许可证。
