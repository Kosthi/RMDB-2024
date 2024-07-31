/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "common/config.h"
#include "rwlatch.h"
#include <cstring>

/**
 * @description: 存储层每个Page的id的声明
 */
struct PageId {
    int fd; //  Page所在的磁盘文件开启后的文件描述符, 来定位打开的文件在内存中的位置
    page_id_t page_no = INVALID_PAGE_ID;

    friend bool operator==(const PageId &x, const PageId &y) { return x.fd == y.fd && x.page_no == y.page_no; }

    bool operator<(const PageId &x) const {
        if (fd < x.fd) return true;
        return page_no < x.page_no;
    }

    std::string toString() {
        return "{fd: " + std::to_string(fd) + " page_no: " + std::to_string(page_no) + "}";
    }

    // inline int64_t Get() const {
    //     return (static_cast<int64_t>(fd << 16) | page_no);
    // }
};

// PageId的自定义哈希算法, 用于构建unordered_map<PageId, frame_id_t, PageIdHash>
// struct PageIdHash {
//     size_t operator()(const PageId &x) const { return (x.fd << 16) | x.page_no; }
// };

namespace std {
    template<>
    struct hash<PageId> {
        size_t operator()(const PageId &obj) const {
            std::size_t h1 = std::hash<int>{}(obj.fd);
            std::size_t h2 = std::hash<page_id_t>{}(obj.page_no);
            return h1 ^ (h2 << 1);
        }
    };
}

/**
 * @description: Page类声明, Page是RMDB数据块的单位、是负责数据操作Record模块的操作对象，
 * Page对象在磁盘上有文件存储, 若在Buffer中则有帧偏移, 并非特指Buffer或Disk上的数据
 */
class Page {
    friend class BufferPoolInstance;

public:
    Page() { reset_memory(); }

    ~Page() = default;

    inline PageId get_page_id() const { return id_; }

    inline int get_page_fd() const { return id_.fd; }

    inline int get_page_no() const { return id_.page_no; }

    inline char *get_data() { return data_; }

    inline bool is_dirty() const { return is_dirty_; }

    static constexpr size_t OFFSET_PAGE_START = 0;
    static constexpr size_t OFFSET_LSN = 0;
    static constexpr size_t OFFSET_PAGE_HDR = 4;

    inline lsn_t get_page_lsn() { return *reinterpret_cast<lsn_t *>(get_data() + OFFSET_LSN); }

    inline void set_page_lsn(lsn_t page_lsn) { memcpy(get_data() + OFFSET_LSN, &page_lsn, sizeof(lsn_t)); }

    inline void WLatch() {
        // rwlatch_.WLock();
    }

    inline void WUnlatch() {
        // rwlatch_.WUnlock();
    }

    inline void RLatch() {
        // rwlatch_.RLock();
    }

    inline void RUnlatch() {
        // rwlatch_.RUnlock();
    }

    inline int get_pin_count() const { return pin_count_; }

private:
    void reset_memory() {
        // 将 data_ 的 PAGE_SIZE 个字节填充为 0
        memset(data_, OFFSET_PAGE_START, PAGE_SIZE);
        // 设置初始 lsn
        set_page_lsn(INVALID_LSN);
    }

    /** page的唯一标识符 */
    PageId id_;

    /** The actual data that is stored within a page.
     *  该页面在bufferPool中的偏移地址
     */
    char data_[PAGE_SIZE] = {};

    /** 脏页判断 */
    bool is_dirty_ = false;

    /** The pin count of this page. */
    int pin_count_ = 0;

    /** 页读写锁 */
    RWLatch rwlatch_;
};
