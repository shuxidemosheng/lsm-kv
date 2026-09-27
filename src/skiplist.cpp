// skiplist.cpp —— 跳表实现
//
// 跳表的本质：给有序链表加"电梯"。底层(level 0)是包含所有节点的完整链表，
// 上面每层都是下层的"快车"（期望跳过 4 个停一站）。查找时从最高层出发，
// 能快进就快进，走到头再下一层——期望时间 O(log n)。
#include "skiplist.h"

#include <random>

namespace lsm {

SkipList::SkipList()
    : head_(new Node("", "", false, kMaxLevel)),
      level_(1),
      size_bytes_(0) {}

SkipList::~SkipList() {
    // 链表逐个释放：沿 level0 一路 next 即可（其他层的指针都会随之消失）
    Node* p = head_;
    while (p) {
        Node* nxt = p->next[0];
        delete p;
        p = nxt;
    }
}

int SkipList::RandomLevel() {
    // 每层以 1/kBranch 的概率继续升高。用 <random> 而不是 rand()：
    // 线程安全性无所谓（MemTable 单线程写），但分布质量要稳定。
    static thread_local std::mt19937 rng(std::random_device{}());
    static thread_local std::uniform_int_distribution<int> dist(0, kBranch - 1);

    int level = 1;
    while (level < kMaxLevel && dist(rng) == 0)   // dist==0 概率 1/4：再升一层
        ++level;
    return level;
}

SkipList::Node* SkipList::FindGreaterOrEqual(const std::string& key,
                                             Node* prev[]) const {
    Node* p = head_;
    // 从当前最高层开始，自上而下、自左向右地逼近目标
    for (int i = level_ - 1; i >= 0; --i) {
        // 在第 i 层向右走，直到下一个节点 key >= 查找 key
        while (p->next[i] != nullptr && p->next[i]->key < key)
            p = p->next[i];
        if (prev) prev[i] = p;        // 记录第 i 层的前驱（插入时用）
    }
    return p->next[0];                // level0 上第一个 >= key 的节点
}

void SkipList::Put(const std::string& key, const std::string& value,
                   bool tombstone) {
    Node* prev[kMaxLevel];
    Node* found = FindGreaterOrEqual(key, prev);

    // 情形一：key 已存在 —— 原地覆盖，调整记账
    if (found != nullptr && found->key == key) {
        size_bytes_ += value.size() - found->value.size();
        found->value = value;
        found->tombstone = tombstone;
        return;
    }

    // 情形二：新 key —— 抛硬币定层数，创建节点，逐层插入链表
    int lvl = RandomLevel();
    if (lvl > level_) {
        // 新节点比当前最高层还高：空头层的前驱都是 head_
        for (int i = level_; i < lvl; ++i) prev[i] = head_;
        level_ = lvl;
    }

    Node* node = new Node(key, value, tombstone, lvl);
    for (int i = 0; i < lvl; ++i) {
        node->next[i] = prev[i]->next[i];
        prev[i]->next[i] = node;
    }

    // 记账：key/value 字符串本体 + 节点结构 + 指针的近似开销
    size_bytes_ += key.size() + value.size() + sizeof(Node) +
                   lvl * sizeof(Node*) + 32;
}

bool SkipList::Get(const std::string& key, std::string* value,
                   bool* tombstone) const {
    // 查询不需要记录前驱，传 nullptr 即可
    Node* found = FindGreaterOrEqual(key, nullptr);
    if (found == nullptr || found->key != key)
        return false;                        // 查无此 key
    if (value) *value = found->value;
    if (tombstone) *tombstone = found->tombstone;
    return true;
}

}  // namespace lsm
