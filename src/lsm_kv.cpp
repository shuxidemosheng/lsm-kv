// lsm_kv.cpp —— 引擎实现（阶段 2：读写路径；阶段 3 加 compaction）
#include "lsm_kv.h"

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>

namespace lsm {

namespace {

// WAL 记录格式：u32 klen | u32 vlen | u8 tombstone | key | value
bool WalAppend(int fd, const std::string& key, const std::string& value,
               bool tomb, bool sync) {
    std::string buf;
    uint32_t klen = static_cast<uint32_t>(key.size());
    uint32_t vlen = static_cast<uint32_t>(value.size());
    buf.append(reinterpret_cast<const char*>(&klen), 4);
    buf.append(reinterpret_cast<const char*>(&vlen), 4);
    buf.push_back(static_cast<char>(tomb ? 1 : 0));
    buf.append(key);
    buf.append(value);
    size_t off = 0;
    while (off < buf.size()) {
        ssize_t n = write(fd, buf.data() + off, buf.size() - off);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    // fflush 无意义（write 已直达内核页缓存）；sync 决定是否等掉电安全
    if (sync && fsync(fd) < 0) return false;
    return true;
}

// 从文件名解析 "l0-00000001.sst" / "l1-00000002.sst"
bool ParseSstName(const std::string& name, int* level, uint64_t* id) {
    if (name.size() < 10 || name.substr(name.size() - 4) != ".sst") return false;
    int lvl = name[0] == 'l' ? name[1] - '0' : -1;
    if (lvl != 0 && lvl != 1) return false;
    *id = strtoull(name.c_str() + 3, nullptr, 10);
    *level = lvl;
    return true;
}

}  // namespace

LsmKV* LsmKV::Open(const LsmOptions& opts, std::string* err) {
    auto* kv = new LsmKV();
    kv->opts_ = opts;
    if (mkdir(opts.dir.c_str(), 0755) < 0 && errno != EEXIST) {
        if (err) *err = "mkdir " + opts.dir + ": " + strerror(errno);
        delete kv;
        return nullptr;
    }

    // 1. 扫描目录，恢复 L0/L1 文件列表
    if (!kv->LoadDir(err)) { delete kv; return nullptr; }

    // 2. 打开 WAL 并重放：把宕机前已确认但未 flush 的写恢复进 MemTable。
    //    崩溃恢复的正确性依赖"前缀一致"：记录要么完整要么截断，
    //    半条记录读不出来就停——这就是 LSM 的 write-ahead 语义。
    std::string walpath = opts.dir + "/wal.log";
    kv->wal_fd_ = open(walpath.c_str(), O_RDWR | O_CREAT, 0644);
    if (kv->wal_fd_ < 0) {
        if (err) *err = "open wal: " + std::string(strerror(errno));
        delete kv;
        return nullptr;
    }
    {
        std::string wbuf;
        char tmp[4096];
        ssize_t n;
        while ((n = read(kv->wal_fd_, tmp, sizeof(tmp))) > 0) wbuf.append(tmp, n);
        size_t p = 0;
        while (p + 9 <= wbuf.size()) {
            uint32_t klen, vlen;
            memcpy(&klen, wbuf.data() + p, 4);
            memcpy(&vlen, wbuf.data() + p + 4, 4);
            if (p + 9 + klen + vlen > wbuf.size()) break;   // 半条记录：停
            bool tomb = wbuf[p + 8] != 0;
            std::string k(wbuf, p + 9, klen);
            std::string v(wbuf, p + 9 + klen, vlen);
            kv->memtable_.Put(k, v, tomb);
            kv->memtable_size_ += 9 + klen + vlen;
            p += 9 + klen + vlen;
        }
        if (p < wbuf.size()) {
            // 截掉崩溃时写了一半的尾巴
            if (ftruncate(kv->wal_fd_, static_cast<off_t>(p)) < 0) {}
        }
        lseek(kv->wal_fd_, 0, SEEK_END);   // 追加位置挪到文件尾
    }
    return kv;
}

bool LsmKV::LoadDir(std::string* err) {
    DIR* d = opendir(opts_.dir.c_str());
    if (!d) {
        if (err) *err = "opendir: " + std::string(strerror(errno));
        return false;
    }
    struct dirent* e;
    std::vector<std::pair<uint64_t, std::string>> l0names;
    std::vector<std::pair<uint64_t, std::string>> l1names;   // L1 可能有多个（崩溃残留）
    while ((e = readdir(d)) != nullptr) {
        int lvl;
        uint64_t id;
        if (!ParseSstName(e->d_name, &lvl, &id)) continue;
        next_file_id_ = std::max(next_file_id_, id + 1);
        if (lvl == 0) l0names.push_back({id, e->d_name});
        else l1names.push_back({id, e->d_name});
    }
    closedir(d);

    // L0 按 id 降序 = 新的在前（Get 时先查新文件）
    std::sort(l0names.begin(), l0names.end(),
              [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [id, name] : l0names) {
        SST* s = SST::Open(opts_.dir + "/" + name);
        if (s) l0_.push_back(s);
    }
    // L1 可能有多个（compaction 中途崩溃的残留），从新到旧挑第一个能打开的；
    // 打不开的（半个文件）忽略——它的内容还没生效，旧 L1 兜底
    std::sort(l1names.begin(), l1names.end(),
              [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [id, name] : l1names) {
        l1_ = SST::Open(opts_.dir + "/" + name);
        if (l1_) { next_file_id_ = std::max(next_file_id_, id + 1); break; }
    }
    return true;
}

LsmKV::~LsmKV() {
    for (SST* s : l0_) delete s;
    delete l1_;
    if (wal_fd_ >= 0) close(wal_fd_);
}

bool LsmKV::Put(const std::string& key, const std::string& value) {
    if (!WalAppend(wal_fd_, key, value, false, opts_.sync_wal)) return false;
    memtable_.Put(key, value, false);
    memtable_size_ += 9 + key.size() + value.size();
    if (memtable_size_ >= opts_.memtable_limit) return FlushLocked();
    return true;
}

bool LsmKV::Delete(const std::string& key) {
    if (!WalAppend(wal_fd_, key, "", true, opts_.sync_wal)) return false;
    memtable_.Put(key, "", true);            // 删除 = 写墓碑
    memtable_size_ += 9 + key.size();
    if (memtable_size_ >= opts_.memtable_limit) return FlushLocked();
    return true;
}

bool LsmKV::Get(const std::string& key, std::string* value) {
    // 1. MemTable 最新
    {
        std::string v;
        bool tomb = false;
        if (memtable_.Get(key, &v, &tomb)) {
            if (tomb) return false;          // 最新版本是墓碑 → 已删，停止下探
            *value = v;
            return true;
        }
    }
    // 2. L0：新的文件在前，第一个命中即为最新版本
    for (SST* s : l0_) {
        std::string v;
        bool tomb = false;
        if (s->Get(key, &v, &tomb)) {
            if (tomb) return false;          // 同上：墓碑截断对老文件的查找
            *value = v;
            return true;
        }
    }
    // 3. L1（底层，全是老版本）
    if (l1_) {
        std::string v;
        bool tomb = false;
        if (l1_->Get(key, &v, &tomb)) {
            if (tomb) return false;
            *value = v;
            return true;
        }
    }
    return false;
}

bool LsmKV::FlushLocked() {
    if (memtable_size_ == 0) return true;
    char name[64];
    snprintf(name, sizeof(name), "l0-%08llu.sst",
             static_cast<unsigned long long>(next_file_id_));
    std::string path = opts_.dir + "/" + name;

    SSTBuilder b(path);
    memtable_.ForEach([&](const Entry& e) { b.Add(e); });
    b.Finish();

    SST* s = SST::Open(path);
    if (!s) return false;
    l0_.insert(l0_.begin(), s);              // 新文件插最前（ newest first）
    ++next_file_id_;

    // MemTable 清空：WAL 从头开始写
    memtable_.Clear();
    memtable_size_ = 0;
    if (ftruncate(wal_fd_, 0) < 0) return false;
    lseek(wal_fd_, 0, SEEK_END);

    // 阶段 3 的钩子：L0 文件数超阈值 → 触发 compaction
    if (l0_.size() >= opts_.l0_compact_trigger) return CompactLocked();
    return true;
}

// 阶段 3：k 路归并 L0(+L1) → 新 L1
//
// 归并规则：
//   - 各输入文件内部 key 有序且唯一；文件间可能同 key；
//   - 输入按"新→旧"排列，同 key 时取最先出现的（最新版本），
//     其余版本跳过；
//   - 输出目标是**底层**（L1 之下没有更老的数据），所以墓碑可以
//     物理删除——这正是"删除被推迟到 compaction"的兑现处。
bool LsmKV::CompactLocked() {
    std::vector<SST*> inputs = l0_;          // 已按新→旧排列
    if (l1_) inputs.push_back(l1_);          // L1 最老，排最后

    std::vector<SST::Iter> iters;
    iters.reserve(inputs.size());
    for (SST* s : inputs) iters.push_back(s->NewIter());

    char name[64];
    snprintf(name, sizeof(name), "l1-%08llu.sst",
             static_cast<unsigned long long>(next_file_id_));
    std::string newpath = opts_.dir + "/" + name;

    SSTBuilder b(newpath);
    size_t kept = 0, dropped_tomb = 0, dropped_dup = 0;
    for (;;) {
        // 在所有有效迭代器里找最小 key（输入少，线性扫足够）
        int best = -1;
        for (size_t i = 0; i < iters.size(); ++i) {
            if (!iters[i].Valid()) continue;
            if (best < 0 || iters[i].entry().key < iters[best].entry().key)
                best = static_cast<int>(i);
        }
        if (best < 0) break;                 // 全部耗尽
        std::string key = iters[best].entry().key;
        Entry e = iters[best].entry();       // 最新版本（同 key 中最先出现）

        // 所有持有该 key 的迭代器都前进（同 key 的老版本被覆盖/删除）
        for (auto& it : iters)
            if (it.Valid() && it.entry().key == key) it.Next();

        if (e.tombstone) {
            ++dropped_tomb;                  // 底层之下无更老数据 → 墓碑可弃
        } else {
            b.Add(e);
            ++kept;
        }
    }
    b.Finish();

    // 删除旧输入文件，切换到新 L1
    std::vector<std::string> oldpaths;
    for (SST* s : l0_) {
        oldpaths.push_back(s->path());
        delete s;
    }
    l0_.clear();
    if (l1_) {
        oldpaths.push_back(l1_->path());
        delete l1_;
        l1_ = nullptr;
    }
    l1_ = SST::Open(newpath);
    if (!l1_) return false;
    ++next_file_id_;
    for (const auto& p : oldpaths) remove(p.c_str());   // cstdio::remove

    fprintf(stderr, "[compact] kept=%zu drop_tomb=%zu drop_dup=%zu -> %s\n",
            kept, dropped_tomb, dropped_dup, name);
    return true;
}

}  // namespace lsm
