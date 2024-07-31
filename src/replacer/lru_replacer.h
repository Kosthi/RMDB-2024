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

#include <list>
#include <mutex>
#include <vector>

#include "common/config.h"
#include "replacer/replacer.h"
#include "unordered_map"

/*
LRUReplacer实现了LRU替换策略
*/
class LRUReplacer : public Replacer {
public:
    /**
     * @description: 创建一个新的LRUReplacer
     * @param {size_t} num_pages LRUReplacer最多需要存储的page数量
     */
    explicit LRUReplacer(size_t num_pages);

    ~LRUReplacer();

    bool victim(frame_id_t *frame_id);

    void pin(frame_id_t frame_id);

    void unpin(frame_id_t frame_id);

    size_t Size();

private:
    std::mutex latch_; // 互斥锁
    std::list<frame_id_t> LRUlist_; // 按加入的时间顺序存放unpinned pages的frame id，首部表示最近被访问
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> LRUhash_; // frame_id_t -> unpinned pages的frame id
    size_t max_size_; // 最大容量（与缓冲池的容量相同）
};

class ClockReplacer : public Replacer {
public:
    /**
     * @description: 创建一个新的LRUReplacer
     * @param {size_t} num_pages LRUReplacer最多需要存储的page数量
     */
    explicit ClockReplacer() {
    }

    ~ClockReplacer() {
    }

    bool victim(frame_id_t *frame_id) override {
        int steps = 0;
        do {
            pointer_ = (pointer_ + 1) % BUFFER_POOL_INSTANCE_SIZE;
            if (pin_counter_[pointer_] == 0 && pin_[pointer_] == 0) {
                *frame_id = pointer_;
                return true;
            }
            if (pin_counter_[pointer_] == 0) {
                pin_[pointer_] = false;
            }
            ++steps;
        } while (steps < 2 * BUFFER_POOL_INSTANCE_SIZE);
        return false;
    }

    void pin(frame_id_t frame_id) override {
        ++pin_counter_[frame_id];
        if (pin_counter_[frame_id] == 1)
            pin_[frame_id] = true;
    }

    void unpin(frame_id_t frame_id) override {
        --pin_counter_[frame_id];
    }

    int get_pin_count(frame_id_t frame_id) { return pin_counter_[frame_id]; }

    size_t Size() override { return BUFFER_POOL_INSTANCE_SIZE; }

private:
    int pin_counter_[BUFFER_POOL_INSTANCE_SIZE];
    bool pin_[BUFFER_POOL_INSTANCE_SIZE];
    int pointer_ = 0;
};
