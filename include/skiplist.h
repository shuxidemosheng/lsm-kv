// skiplist.h —— 跳表：MemTable 的存储结构（阶段 0 的核心）
//
// 为什么 MemTable 用跳表而不用红黑树/平衡树？
//   1. 实现简单、无旋转，插入只做"抛硬币定层数 + 改指针"；
//   2. 有序性天然支持范围扫描（flush 成 SST 时要按 key 升序倒出）；
//   3. 并发实现容易（LevelDB 的 SkipList 只用原子指针就能无锁读），
//      平衡树的旋转在并发下要复杂得多。
//
// 语义约定（与 LSM 语义对齐）：
//   - 同一个 key 重复 Put = 覆盖（最后一次写生效）；
//   - 删除不真删，而是写入 tombstone（删除标记）——因为旧版本可能
//     还躺在磁盘的 SST 里，内存里必须留下"此 key 已删"的证据。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace lsm {

// 一条 KV 记录。tombstone=true 表示"该 key 已被删除"
struct Entry {
    std::string key;
    std::string value;
    bool tombstone = false;
};

class SkipList {
public:
    SkipList();
    ~SkipList();
    SkipList(const SkipList&) = delete;             // 持有裸指针，禁止拷贝
    SkipList& operator=(const SkipList&) = delete;

    // 写入/覆盖一条记录。tombstone=true 表示写入删除标记
    void Put(const std::string& key, const std::string& value, bool tombstone = false);

    // 查询。返回是否找到；*tombstone 告诉调用方这条是不是删除标记
    // （调用方必须把"找到但是墓碑"当作"查无此 key"，同时记住
    //   更老层的 SST 里即使有也不能再看——这是 LSM 读路径的关键规则）
    bool Get(const std::string& key, std::string* value, bool* tombstone) const;

    // 清空所有记录（flush 后重置 MemTable 用）
    void Clear();

    // 近似内存占用（字节）。MemTable 用它决定何时 flush 成 SST。
    // "近似"即可：精确记账得不偿失，LevelDB 也只记近似值。
    size_t ApproximateSize() const { return size_bytes_; }

    // 按 key 升序遍历全部记录（flush 时逐条写入 SST）。
    // 用模板回调而不是定义迭代器类：阶段 0 保持简单。
    template <typename Fn>
    void ForEach(Fn&& fn) const {
        for (Node* p = head_->next[0]; p != nullptr; p = p->next[0])
            fn(Entry{p->key, p->value, p->tombstone});
    }

    // 测试辅助：当前跳表的逻辑层数
    int MaxLevelNow() const { return level_; }

private:
    static constexpr int kMaxLevel = 16;   // 层数上限；16 层可容纳 4^16 个元素
    static constexpr int kBranch = 4;      // 升层概率 = 1/kBranch，期望每层元素数 ÷4

    struct Node {
        std::string key;
        std::string value;
        bool tombstone;
        std::vector<Node*> next;   // next[i]：本节点在第 i 层的后继指针

        Node(std::string k, std::string v, bool t, int level)
            : key(std::move(k)), value(std::move(v)), tombstone(t),
              next(static_cast<size_t>(level), nullptr) {}
    };

    // 抛硬币决定新节点的层数：每层以 1/4 概率再升一层
    int RandomLevel();

    // 从最高层往下找"最后一个 < key 的节点"，沿途记录每一层的前驱。
    // prev[i] 填好后，插入新节点只需要改各层的 prev[i]->next[i]。
    // 返回 level0 上第一个 >= key 的节点（可能为 nullptr）。
    Node* FindGreaterOrEqual(const std::string& key, Node* prev[]) const;

    Node* head_;          // 哨兵头节点（不存数据，拥有 kMaxLevel 层指针）
    int level_;           // 当前实际最高层数（1 起步，随插入增长）
    size_t size_bytes_;   // 近似内存占用
};

}  // namespace lsm
