#include "paths.hpp"

#include <limits.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <cstdlib>
#include <vector>
#endif

#include <string>

namespace {

std::string parent_dir_of(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

}  // namespace

std::string executable_dir() {
#if defined(__APPLE__)
    char stack_buf[PATH_MAX];
    uint32_t size = static_cast<uint32_t>(sizeof(stack_buf));
    std::string raw_path;
    if (_NSGetExecutablePath(stack_buf, &size) == 0) {
        raw_path.assign(stack_buf);
    } else if (size > 0) {
        std::vector<char> dyn_buf(size);
        if (_NSGetExecutablePath(dyn_buf.data(), &size) == 0) {
            raw_path.assign(dyn_buf.data());
        }
    }
    if (raw_path.empty()) {
        return {};
    }
    char resolved[PATH_MAX];
    if (::realpath(raw_path.c_str(), resolved) != nullptr) {
        return parent_dir_of(resolved);
    }
    return parent_dir_of(raw_path);
#else
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return {};
    }
    buf[n] = '\0';
    return parent_dir_of(std::string(buf, static_cast<std::size_t>(n)));
#endif
}
