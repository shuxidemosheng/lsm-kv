// sst.cpp —— SST 构建与读取
//
// 编码全部手写（小端、固定宽度），不依赖任何序列化库——
// 一来零依赖，二来复试能讲清"磁盘上一个字节长什么样"。
#include "sst.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace lsm {

namespace {

void AppendU32(std::string* buf, uint32_t v) {
    char b[4];
    memcpy(b, &v, 4);                          // x86/WSL 小端，直接 memcpy
    buf->append(b, 4);
}
void AppendU64(std::string* buf, uint64_t v) {
    char b[8];
    memcpy(b, &v, 8);
    buf->append(b, 8);
}
bool ReadU32(const char* p, uint32_t* v) { memcpy(v, p, 4); return true; }
bool ReadU64(const char* p, uint64_t* v) { memcpy(v, p, 8); return true; }

constexpr char kMagic[4] = {'L', 'K', 'V', 'S'};

}  // namespace

// ---------------- Builder ----------------

SSTBuilder::SSTBuilder(std::string path) : path_(std::move(path)) {
    fd_ = open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

SSTBuilder::~SSTBuilder() {
    if (fd_ >= 0) close(fd_);
}

void SSTBuilder::WriteRecord(const Entry& e) {
    std::string buf;
    buf.reserve(9 + e.key.size() + e.value.size());
    buf.push_back(static_cast<char>(e.tombstone ? 1 : 0));
    AppendU32(&buf, static_cast<uint32_t>(e.key.size()));
    AppendU32(&buf, static_cast<uint32_t>(e.value.size()));
    buf.append(e.key);
    buf.append(e.value);
    ssize_t n = write(fd_, buf.data(), buf.size());
    (void)n;
}

void SSTBuilder::Add(const Entry& e) {
    if (num_entries_ == 0) {
        // 第一个索引点：文件起点
        index_.push_back({e.key, offset_});
        bytes_since_index_ = 0;
    } else if (bytes_since_index_ >= kIndexInterval) {
        // 攒够 ~4KB 数据，记一个新的索引点（该块第一条 entry）
        index_.push_back({e.key, offset_});
        bytes_since_index_ = 0;
    }
    keys_.push_back(e.key);        // 布隆过滤器的原料（Finish 时统一构建）
    WriteRecord(e);
    offset_ += 9 + e.key.size() + e.value.size();  // 1+4+4+klen+vlen
    bytes_since_index_ += 9 + e.key.size() + e.value.size();
    ++num_entries_;
}

void SSTBuilder::Finish() {
    uint64_t bloom_offset = offset_;

    // 1. 布隆过滤区（预期元素数 = 实际 key 数，目标误判率 1%）
    std::string bloom;
    uint32_t bloom_len = 0;
    if (!keys_.empty()) {
        BloomFilter bf(keys_.size(), 0.01);
        for (const auto& k : keys_) bf.Add(k);
        bloom = bf.Serialize();
        bloom_len = static_cast<uint32_t>(bloom.size());
        ssize_t nb = write(fd_, bloom.data(), bloom.size());
        (void)nb;
        offset_ += bloom.size();
    }

    uint64_t index_offset = offset_;

    // 2. 稀疏索引区
    std::string idx;
    for (const auto& ie : index_) {
        AppendU32(&idx, static_cast<uint32_t>(ie.key.size()));
        idx.append(ie.key);
        AppendU64(&idx, ie.offset);
    }
    ssize_t n = write(fd_, idx.data(), idx.size());
    (void)n;

    // 3. footer（固定 40 字节）
    std::string ft;
    ft.append(kMagic, 4);
    AppendU32(&ft, kSstVersion);
    AppendU64(&ft, num_entries_);
    AppendU64(&ft, index_offset);
    AppendU32(&ft, static_cast<uint32_t>(index_.size()));
    AppendU64(&ft, bloom_offset);
    AppendU32(&ft, bloom_len);
    n = write(fd_, ft.data(), ft.size());
    (void)n;

    fsync(fd_);                 // SST 是"flush 完成"的凭证，必须真落盘
    close(fd_);
    fd_ = -1;
    finished_ = true;
}

// ---------------- 只读视图 ----------------

bool SST::ReadExact(uint64_t off, void* buf, size_t len) const {
    size_t got = 0;
    while (got < len) {
        ssize_t r = pread(fd_, static_cast<char*>(buf) + got,
                          len - got, static_cast<off_t>(off + got));
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

SST* SST::Open(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;

    // 1. 读 footer（文件末尾固定 40 字节）
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < static_cast<off_t>(kSstFooterSize)) {
        close(fd);
        return nullptr;
    }
    char ft[kSstFooterSize];
    if (pread(fd, ft, kSstFooterSize, st.st_size - kSstFooterSize)
        != kSstFooterSize) {
        close(fd);
        return nullptr;
    }
    if (memcmp(ft, kMagic, 4) != 0) { close(fd); return nullptr; }

    auto* sst = new SST();
    sst->path_ = path;
    sst->fd_ = fd;
    uint32_t version;
    ReadU32(ft + 4, &version);
    ReadU64(ft + 8, &sst->num_entries_);
    ReadU64(ft + 16, &sst->index_offset_);
    uint32_t num_index;
    ReadU32(ft + 24, &num_index);
    ReadU64(ft + 28, &sst->bloom_offset_);
    ReadU32(ft + 36, &sst->bloom_len_);
    sst->data_end_ = sst->index_offset_;
    (void)version;

    // 3. 读布隆过滤区（存在且合法时启用；旧格式/无布隆则跳过）
    if (sst->bloom_len_ > 0 && sst->bloom_offset_ + sst->bloom_len_
        <= sst->index_offset_) {
        std::string bbuf(sst->bloom_len_, 0);
        if (sst->ReadExact(sst->bloom_offset_, bbuf.data(), bbuf.size())) {
            sst->bloom_ = BloomFilter::Deserialize(bbuf);
            sst->has_bloom_ = sst->bloom_.num_bits() > 0;
        }
    }

    // 4. 读整个稀疏索引进内存（每 4KB 数据一条，内存开销可忽略）
    //    索引区长度 = 文件尾 - footer - index_offset
    std::string ibuf(static_cast<size_t>(st.st_size - kSstFooterSize - sst->index_offset_), 0);
    if (!sst->ReadExact(sst->index_offset_, ibuf.data(), ibuf.size())) {
        delete sst;
        return nullptr;
    }
    size_t p = 0;
    sst->index_.reserve(num_index);
    for (uint32_t i = 0; i < num_index; ++i) {
        uint32_t klen;
        memcpy(&klen, ibuf.data() + p, 4);
        p += 4;
        IndexEntry ie;
        ie.key.assign(ibuf, p, klen);
        p += klen;
        memcpy(&ie.offset, ibuf.data() + p, 8);
        p += 8;
        sst->index_.push_back(std::move(ie));
    }
    if (!sst->index_.empty()) sst->smallest_ = sst->index_.front().key;
    return sst;
}

SST::~SST() {
    if (fd_ >= 0) close(fd_);
}

bool SST::ParseRecord(uint64_t off, Entry* e, uint64_t* next_off) const {
    char head[9];
    if (!ReadExact(off, head, 9)) return false;
    e->tombstone = head[0] != 0;
    uint32_t klen, vlen;
    memcpy(&klen, head + 1, 4);
    memcpy(&vlen, head + 5, 4);
    std::string kv(klen + vlen, 0);
    if (!ReadExact(off + 9, kv.data(), klen + vlen)) return false;
    e->key.assign(kv, 0, klen);
    e->value.assign(kv, klen, vlen);
    *next_off = off + 9 + klen + vlen;
    return true;
}

bool SST::Get(const std::string& key, std::string* value, bool* tombstone) const {
    // 第一道闸门：布隆过滤器说"一定不存在"，直接返回
    // （布隆只会误判"存在"，绝不会漏判"不存在"——方向性是它安全性的关键）
    if (has_bloom_ && !bloom_.MayContain(key)) return false;

    // 快速排除：key 小于文件最小 key（= 第一个索引点），必然不存在
    if (index_.empty() || key < index_.front().key)
        return false;

    // 二分稀疏索引：找最后一个 first_key <= 目标 key 的索引点
    size_t lo = 0, hi = index_.size() - 1, hit = 0;
    while (lo <= hi) {
        size_t mid = (lo + hi) / 2;
        if (index_[mid].key <= key) { hit = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    // 目标 key 一定落在 [index_[hit].offset, 下一个索引点或 data_end) 内
    uint64_t off = index_[hit].offset;
    uint64_t end = (hit + 1 < index_.size()) ? index_[hit + 1].offset : data_end_;

    // 在块内顺序扫描（块只有 ~4KB，代价可控）
    while (off < end) {
        Entry e;
        uint64_t next;
        if (!ParseRecord(off, &e, &next)) return false;
        if (e.key == key) {
            if (value) *value = e.value;
            if (tombstone) *tombstone = e.tombstone;
            return true;
        }
        if (e.key > key) return false;   // 已越过目标 → 不存在
        off = next;
    }
    return false;
}

// ---------------- 迭代器 ----------------

SST::Iter::Iter(const SST* sst) : sst_(sst) {
    if (sst_->num_entries_ > 0) {
        offset_ = 0;
        Fill();
    }
}

void SST::Iter::Fill() {
    uint64_t next;
    valid_ = sst_->ParseRecord(offset_, &cur_, &next);
    if (valid_) offset_ = next;
}

void SST::Iter::Next() {
    if (offset_ < sst_->data_end_) Fill();
    else valid_ = false;
}

}  // namespace lsm
