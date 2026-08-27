#pragma once

#include <string>
#include <string_view>
#include <deque>
#include <cstddef>
#include <algorithm>
#include <stdexcept>
#include <limits>
// 不同于其他代码，此处涉及核心安全，注释是非常重要的

/**
 * @brief 分块字符串池
 * 
 * 将字符串按组（块）存储，每组是一个独立的 std::string 对象。
 * 当当前组剩余空间不足以容纳新字符串时，自动创建新组。
 * 所有已返回的 std::string_view 永久有效，不会因后续插入而失效。
 * 
 * 不提供去重功能
 * 不提供迭代器
 * 线程不安全
 */
class StringPool {
public:
    using StringView = std::string_view;
    using SizeType = std::size_t;

    /**
     * @brief 构造函数
     * @param chunk_size 每个组的初始容量（字节），默认 1MB
     */
    explicit StringPool(SizeType chunk_size = 1024 * 1024)
        : default_chunk_size_(chunk_size),
          total_used_(0),
          total_capacity_(0) {
        if (chunk_size == 0) {
            throw std::invalid_argument("chunk_size must be positive");
        }
        add_new_chunk();  // 创建第一个组
    }

    // 禁止拷贝
    StringPool(const StringPool&) = delete;
    StringPool& operator=(const StringPool&) = delete;

    // 允许移动
    StringPool(StringPool&& other) noexcept = default;
    StringPool& operator=(StringPool&& other) noexcept = default;

    /**
     * @brief 插入一个字符串
     * @param str 要插入的字符串（可隐式转换为 std::string_view）
     * @return std::string_view 指向池内存储的字符串
     * 
     * 如果当前组剩余空间不足，会自动创建新组。
     * 空字符串会被特殊处理，返回一个指向静态空字符串的视图。
     */
    StringView insert(std::string_view str) {
        if (str.empty()) {
            static const char empty[] = "";
            return StringView(empty, 0);
        }

        // 需要的总大小：字符串长度 + 1 字节 null 终止符（用于调试）
        SizeType needed = str.size() + 1;

        // 确保当前组有足够空间
        if (!current_chunk_ || current_chunk_->capacity() - current_chunk_->size() < needed) {
            // 新组大小取 max(default_chunk_size_, needed) 并适当放大以减少组数
            SizeType new_chunk_size = std::max(default_chunk_size_, needed);
            // 整数方式放大 1.2 倍，避免浮点溢出
            new_chunk_size += new_chunk_size / 5;   // +20%
            new_chunk_size += 1024;
            // 防止溢出（安全回退）
            if (new_chunk_size < needed) {
                new_chunk_size = needed;
            }
            add_new_chunk(new_chunk_size);
        }

        // 在当前组中追加
        auto& chunk = *current_chunk_;
        SizeType offset = chunk.size();
        chunk.append(str.data(), str.size());
        chunk.push_back('\0');  // 添加 null 终止符（不包含在 string_view 中）

        // 更新统计
        total_used_ += str.size() + 1;

        // 返回视图
        return StringView(chunk.data() + offset, str.size());
    }

    /**
     * @brief 预留空间，确保当前组在未来能容纳至少 total_size 字节的字符串（不包括 null 终止符）
     * @param total_size 期望在**当前组**中连续可用的字节数
     * 
     * 如果当前组剩余空间不足，会创建一个新组，其容量至少为 total_size。
     * 这不会使已返回的 string_view 失效，因为旧组不变。
     * 建议在插入大字符串前调用，以避免频繁创建组。
     */
    void reserve(SizeType total_size) {
        // 检查当前组剩余空间是否足够（包括 null 终止符）
        SizeType needed = total_size + 1; // 预留时也考虑 null
        if (current_chunk_ && current_chunk_->capacity() - current_chunk_->size() >= needed) {
            return; // 当前组空间足够
        }

        // 当前组不够，新建一个足够大的组
        SizeType new_cap = std::max(default_chunk_size_, needed);
        // 略微放大以减少组数
        new_cap += new_cap / 5;
        new_cap += 1024;
        if (new_cap < needed) new_cap = needed;
        add_new_chunk(new_cap);
    }

    /// 获取当前已使用的字节数（包括每个字符串末尾的 null 终止符）
    SizeType used_bytes() const noexcept { return total_used_; }

    /// 获取当前总容量（所有组容量之和）
    SizeType capacity_bytes() const noexcept { return total_capacity_; }

    /// 获取组的数量
    SizeType chunk_count() const noexcept { return chunks_.size(); }

    /// 获取当前组中剩余可用字节数
    SizeType current_free_bytes() const {
        if (!current_chunk_) return 0;
        return current_chunk_->capacity() - current_chunk_->size();
    }

    /**
     * @brief 清空所有数据，释放所有组，恢复到初始状态
     */
    void clear() {
        chunks_.clear();
        current_chunk_ = nullptr;
        total_used_ = 0;
        total_capacity_ = 0;
        add_new_chunk();  // 重新创建第一个组
    }

    /**
     * @brief 获取统计信息
     */
    struct Stats {
        SizeType chunk_count;          // 组数
        SizeType used_bytes;           // 已用字节（含每个字符串末尾的 null）
        SizeType capacity_bytes;       // 总容量
        SizeType wasted_bytes;         // 浪费的字节（容量 - 已用）
        double utilization;            // 利用率（0~1）
        SizeType average_chunk_size;   // 平均每组容量
    };

    Stats get_stats() const {
        Stats stats;
        stats.chunk_count = chunks_.size();
        stats.used_bytes = total_used_;
        stats.capacity_bytes = total_capacity_;
        stats.wasted_bytes = stats.capacity_bytes - stats.used_bytes;
        stats.utilization = stats.capacity_bytes > 0
            ? static_cast<double>(stats.used_bytes) / stats.capacity_bytes
            : 0.0;
        stats.average_chunk_size = stats.chunk_count > 0
            ? stats.capacity_bytes / stats.chunk_count
            : 0;
        return stats;
    }

private:
    using Chunk = std::string;
    using ChunkContainer = std::deque<Chunk>;   // 使用 deque，保证元素地址稳定

    SizeType default_chunk_size_;
    ChunkContainer chunks_;          // 所有组
    Chunk* current_chunk_ = nullptr; // 当前活动的组（指向 chunks_.back()）
    SizeType total_used_ = 0;
    SizeType total_capacity_ = 0;

    /**
     * @brief 创建一个新组，并设为当前组
     * @param capacity 新组的初始容量，若为 0 则使用默认大小
     */
    void add_new_chunk(SizeType capacity = 0) {
        if (capacity == 0) {
            capacity = default_chunk_size_;
        }
        // 确保容量至少为 1
        if (capacity < 1) capacity = 1;

        // 直接在 deque 末尾构造 Chunk 并预留
        chunks_.emplace_back();
        Chunk& new_chunk = chunks_.back();
        new_chunk.reserve(capacity);

        // 累加实际容量（reserve 可能分配略多）
        total_capacity_ += new_chunk.capacity();

        current_chunk_ = &new_chunk;
    }
};
