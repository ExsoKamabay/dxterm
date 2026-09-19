#include "vfs/guest_path.hpp"

namespace vhdp::vfs {

bool is_absolute(std::string_view p) noexcept {
    return !p.empty() && p.front() == '/';
}

std::vector<std::string_view> components(std::string_view p) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == '/') {
            ++i;
        }
        std::size_t start = i;
        while (i < p.size() && p[i] != '/') {
            ++i;
        }
        std::string_view c = p.substr(start, i - start);
        if (!c.empty() && c != ".") {
            out.push_back(c);
        }
    }
    return out;
}

bool has_dotdot(std::string_view p) {
    for (auto c : components(p)) {
        if (c == "..") {
            return true;
        }
    }
    return false;
}

std::string normalize_lexical(std::string_view abs) {
    std::vector<std::string_view> stack;
    for (auto c : components(abs)) {
        if (c == "..") {
            if (!stack.empty()) {
                stack.pop_back();
            }
            continue;
        }
        stack.push_back(c);
    }
    if (stack.empty()) {
        return "/";
    }
    std::string out;
    for (auto c : stack) {
        out.push_back('/');
        out.append(c);
    }
    return out;
}

std::string join(std::string_view base, std::string_view rel) {
    if (is_absolute(rel)) {
        return std::string(rel);
    }
    std::string out(base);
    if (out.empty() || out.back() != '/') {
        out.push_back('/');
    }
    out.append(rel);
    return out;
}

bool is_below(std::string_view path, std::string_view prefix) noexcept {
    if (prefix == "/") {
        return is_absolute(path);
    }
    if (path.size() < prefix.size() || path.substr(0, prefix.size()) != prefix) {
        return false;
    }
    return path.size() == prefix.size() || path[prefix.size()] == '/';
}

std::string_view suffix_after(std::string_view path, std::string_view prefix) noexcept {
    if (prefix == "/") {
        return path == "/" ? std::string_view{} : path;
    }
    return path.substr(prefix.size());
}

} // namespace vhdp::vfs
