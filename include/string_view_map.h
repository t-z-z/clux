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
        return std::hash<std::string_view>{}(sv);
    }
    std::size_t operator()(const std::string& sv) const noexcept {
        return std::hash<std::string>{}(sv);
    }
};

struct StringViewEqual {
    using is_transparent = void;
    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
    bool operator()(const std::string& lhs, const std::string& rhs) const noexcept {
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

// 字符串视图集合（元素即键，固定为 std::string_view，无需额外模板参数）
using svset = std::unordered_set<
    std::string_view,
    StringViewHash,
    StringViewEqual
>;
