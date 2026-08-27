#pragma once
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <string>
#include <memory>
#include <type_traits>


struct StringViewHash {
    using is_transparent = void;  // 启用透明查找
    std::size_t operator()(std::string_view sv) const noexcept {
        // 使用标准哈希
        return std::hash<std::string_view>{}(sv);
    }
};

struct StringViewEqual {
    using is_transparent = void;
    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
};

template<typename Value>
using svmap = std::unordered_map<
    std::string_view,
    Value,
    StringViewHash,
    StringViewEqual
>;

template<typename Value>
using svset = std::unordered_set<
    std::string_view,
    Value,
    StringViewHash,
    StringViewEqual
>;
