// bloom.h —— 布隆过滤器：SST 点查的快速排除器
//
// 原理：m 位的位数组 + k 个哈希函数。插入 key 时把 k 个哈希位都置 1；
// 查询 key 时 k 个位"全为 1"才可能存在（可能误判存在），任一位为 0 则
// 一定不存在。false positive 率 p ≈ (1-e^(-kn/m))^k。
// 用途：get 一个不存在的 key 时，避免把 SST 的索引/数据块读出来——
// LSM 读放大的第一道闸门。
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lsm {

class BloomFilter {
public:
    BloomFilter() = default;

    // expected：预期元素数；fpr：目标误判率
    BloomFilter(size_t expected, double fpr) {
        // 最优位数 m = -n*ln(p)/(ln2)^2；最优哈希数 k = m/n*ln2
        double m = -static_cast<double>(expected) * log(fpr) / (log(2.0) * log(2.0));
        num_bits_ = static_cast<uint32_t>(m) + 1;
        num_hashes_ = static_cast<uint32_t>(
            (static_cast<double>(num_bits_) / expected) * log(2.0) + 0.5);
        if (num_hashes_ < 1) num_hashes_ = 1;
        if (num_hashes_ > 30) num_hashes_ = 30;
        bits_.assign((num_bits_ + 7) / 8, 0);
    }

    void Add(const std::string& key) {
        uint64_t h1, h2;
        HashPair(key, &h1, &h2);
        for (uint32_t i = 0; i < num_hashes_; ++i) {
            // double hashing：k 个哈希 = h1 + i*h2，只需两个真哈希
            uint64_t pos = (h1 + i * h2) % num_bits_;
            bits_[pos >> 3] |= 1u << (pos & 7);
        }
    }

    bool MayContain(const std::string& key) const {
        uint64_t h1, h2;
        HashPair(key, &h1, &h2);
        for (uint32_t i = 0; i < num_hashes_; ++i) {
            uint64_t pos = (h1 + i * h2) % num_bits_;
            if (!(bits_[pos >> 3] & (1u << (pos & 7)))) return false;
        }
        return true;    // 全为 1：大概率存在（可能误判）
    }

    // 序列化：u32 位数 | u32 哈希数 | 位数组（整体写进 SST 的 bloom 区）
    std::string Serialize() const {
        std::string s;
        s.append(reinterpret_cast<const char*>(&num_bits_), 4);
        s.append(reinterpret_cast<const char*>(&num_hashes_), 4);
        s.append(reinterpret_cast<const char*>(bits_.data()), bits_.size());
        return s;
    }
    static BloomFilter Deserialize(const std::string& s) {
        BloomFilter b;
        if (s.size() < 8) return b;
        memcpy(&b.num_bits_, s.data(), 4);
        memcpy(&b.num_hashes_, s.data() + 4, 4);
        b.bits_.assign((b.num_bits_ + 7) / 8, 0);
        size_t body = b.bits_.size();
        if (s.size() < 8 + body) { b.bits_.clear(); return b; }
        memcpy(b.bits_.data(), s.data() + 8, body);
        return b;
    }

    uint32_t num_bits() const { return num_bits_; }
    uint32_t num_hashes() const { return num_hashes_; }

private:
    // 两个独立的 64 位哈希：FNV-1a 变体 + 乘法混合。
    // 布隆对哈希要求：均匀、快——密码学强度不需要。
    static void HashPair(const std::string& key, uint64_t* h1, uint64_t* h2) {
        uint64_t a = 1469598103934665603ull;   // FNV-1a 64 偏移基准
        uint64_t b = 14695981039346656037ull;
        for (unsigned char c : key) {
            a ^= c;
            a *= 1099511628211ull;
            b = (b ^ (c * 0x9e3779b97f4a7c15ull)) * 0xff51afd7ed558ccdull;
            b ^= b >> 31;
        }
        *h1 = a;
        *h2 = b | 1;                            // 保证奇数，步长互质更均匀
    }

    uint32_t num_bits_ = 0;                    // 位数组总位数
    uint32_t num_hashes_ = 0;                  // 哈希函数个数 k
    std::vector<uint8_t> bits_;                // 位数组（按字节存）
};

}  // namespace lsm
