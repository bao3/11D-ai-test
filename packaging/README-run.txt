11dim-ai-test linux-amd64 自包含发布包（11D AI Test）

在解压目录中直接运行：

  ./11dim-ai-test --self-test
  ./11dim-ai-test --model <模型名> --api-url <http://主机:端口/v1/chat/completions>

常用云端模式示例：

  ./11dim-ai-test --api-mode vertex --model gemini-4-hydrogen
  GEMINI_API_KEY="AIza..." ./11dim-ai-test --api-mode gemini --model gemini-4-hydrogen
  OPENAI_API_KEY="sk-..." ./11dim-ai-test --api-mode openai --model gpt-5

第 3 维使用 -std=c++17 -pthread 编译并运行模型生成的并发调度代码。
编译器查找顺序：PS5AI_CXX → PS5AI_GXX → PS5AI_TOOLCHAIN/bin/g++ → 本目录 toolchain/bin/g++ → 系统 g++ / clang++ / c++。
若不想使用包内自带的便携 g++，可将 PS5AI_CXX 指向你本机的编译器。

本二进制在 GitHub Actions ubuntu-latest 上构建，静态链接 libstdc++ / libgcc，动态链接系统 glibc 与 libcurl（可用 ldd ./11dim-ai-test 查看）。若运行在较老 glibc 或 Alpine (musl) 上，建议从源码编译。

随附工具链为 xPack GCC 14.3.0-1 linux-x64（遵循 GCC 许可证），下载地址与 SHA-256 校验值见仓库 scripts/fetch-toolchain.sh。
同目录的 SHA256SUMS 用于校验包内文件完整性；GitHub Release 页面另附有针对压缩包本身的 SHA256SUMS。
