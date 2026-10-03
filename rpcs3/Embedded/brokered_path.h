#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace rpcs3::embedded
{
inline bool brokered_relative_parts(std::string_view path, std::string_view prefix,
    std::vector<std::string>& output)
{
    output.clear();
    if (path != prefix && (!path.starts_with(prefix) || path.size() <= prefix.size() || path[prefix.size()] != '/'))
        return false;
    auto relative = path.substr(prefix.size());
    for (size_t start = 0; start < relative.size();)
    {
        auto end = relative.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = relative.size();
        auto part = relative.substr(start, end - start); start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..")
        {
            if (output.empty()) return false;
            output.pop_back(); continue;
        }
        if (part.find(':') != std::string_view::npos || part.find('\0') != std::string_view::npos ||
            part.back() == '.' || part.back() == ' ') return false;
        output.emplace_back(part);
    }
    return true;
}
}
