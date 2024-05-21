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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class ProjectionExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_; // 投影节点的儿子节点
    std::vector<ColMeta> proj_cols_; // 需要投影的字段
    size_t len_; // 字段总长度
    std::vector<size_t> proj_idxs_; // 每个投影的字段在原先表所有字段的索引
    const std::vector<ColMeta> &prev_cols_;

public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev,
                       const std::vector<TabCol> &proj_cols): prev_(std::move(prev)), prev_cols_(prev_->cols()) {
        size_t curr_offset = 0;
        for (auto &proj_col: proj_cols) {
            // 得到需要投影的列在所有列中的位置
            auto &&pos = get_col(prev_cols_, proj_col);
            // 计算偏移量
            proj_idxs_.emplace_back(pos - prev_cols_.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            proj_cols_.emplace_back(col);
        }
        len_ = curr_offset;
    }

    void beginTuple() override { prev_->beginTuple(); }

    void nextTuple() override { prev_->nextTuple(); }

    std::unique_ptr<RmRecord> Next() override {
        // 满足谓词条件的原记录（这里不能使用std::move，字符串乱码？）
        auto &&prev_record = prev_->Next();
        // 要投影的记录
        auto &&proj_record = std::make_unique<RmRecord>(len_);
        for (std::size_t i = 0; i < proj_idxs_.size(); ++i) {
            // 需要投影的字段
            auto &prev_col = prev_cols_[proj_idxs_[i]];
            // 被投影到的字段
            auto &proj_col = proj_cols_[i];
            // 拷贝投影的字段数据
            memcpy(proj_record->data + proj_col.offset, prev_record->data + prev_col.offset, prev_col.len);
        }
        return std::move(proj_record);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const { return prev_->is_end(); }

    // 需要实现
    const std::vector<ColMeta> &cols() const override { return proj_cols_; }

    size_t tupleLen() const override { return len_; }
};
