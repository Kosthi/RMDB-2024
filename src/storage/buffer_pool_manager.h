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

#include <unordered_map>
#include <vector>

#include "page.h"
#include "disk_manager.h"
#include "replacer/lru_replacer.h"
#include "buffer_pool_instance.h"

class LogManager;

class BufferPoolManager {
private:
    size_t pool_size_; // buffer_pool中可容纳页面的个数，即帧的个数
    BufferPoolInstance *instances_[BUFFER_POOL_INSTANCES]{}; // 缓冲池实例
    std::hash<PageId> hasher_;
    // Page *pages_; // buffer_pool中的Page对象数组，在构造空间中申请内存空间，在析构函数中释放，大小为BUFFER_POOL_SIZE
    // std::unordered_map<PageId, frame_id_t> page_table_; // 帧号和页面号的映射哈希表，用于根据页面的PageId定位该页面的帧编号
    // std::list<frame_id_t> free_list_; // 空闲帧编号的链表
    DiskManager *disk_manager_;
    LogManager *log_manager_;
    // Replacer *replacer_; // buffer_pool的置换策略，当前赛题中为LRU置换策略
    // std::mutex latch_; // 用于共享数据结构的并发控制

public:
    BufferPoolManager(size_t pool_size, DiskManager *disk_manager, LogManager *log_manager = nullptr)
        : pool_size_(pool_size), disk_manager_(disk_manager), log_manager_(log_manager) {
        // 共享lru
        // replacer_ = new LRUReplacer(pool_size_);
        for (auto &instance: instances_) {
            instance = new BufferPoolInstance(pool_size / BUFFER_POOL_INSTANCES, disk_manager_, log_manager_);
        }
    }

    ~BufferPoolManager() {
        // delete replacer_;
        for (auto &instance: instances_) {
            delete instance;
        }
    }

    /**
     * @description: 将目标页面标记为脏页
     * @param {Page*} page 脏页
     */
    // static void mark_dirty(Page *page) { page->is_dirty_ = true; }

public:
    Page *fetch_page(PageId page_id);

    bool unpin_page(PageId page_id, bool is_dirty);

    bool flush_page(PageId page_id);

    Page *new_page(PageId *page_id);

    bool delete_page(PageId page_id);

    void flush_all_pages(int fd);

    void flush_all_pages_for_checkpoint(int fd);

    void delete_all_pages(int fd);

    // auto FetchPageBasic(PageId page_id) -> BasicPageGuard;
    //
    // auto FetchPageRead(PageId page_id) -> ReadPageGuard;
    //
    // auto FetchPageWrite(PageId page_id) -> WritePageGuard;
    //
    // auto NewPageGuarded(PageId *page_id) -> BasicPageGuard;

private:
    inline std::size_t get_instance_no(const PageId &page_id) { return 0; }
};
