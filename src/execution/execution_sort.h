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

class SortExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_;
    ColMeta cols_; // 框架中只支持一个键排序，需要自行修改数据结构支持多个键排序
    size_t tuple_num;
    bool is_desc_;
    std::vector<size_t> used_tuple;
    std::unique_ptr<RmRecord> current_tuple_;
    std::deque<std::unique_ptr<RmRecord> > records_;

public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, const TabCol &sel_cols, bool is_desc) {
        prev_ = std::move(prev);
        // cols_ = prev_->get_col_offset(sel_cols);
        cols_ = *get_col(prev_->cols(), sel_cols);
        is_desc_ = is_desc;
        tuple_num = 0;
        used_tuple.clear();
        current_tuple_ = nullptr;
    }

    void beginTuple() override {
        records_.clear();
        // TODO 内存中可能放不下 实现外部排序
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            records_.emplace_back(prev_->Next());
        }
        if (records_.empty()) {
            return;
        }

        std::sort(records_.begin(), records_.end(),
                  [&](std::unique_ptr<RmRecord> &l_rec, std::unique_ptr<RmRecord> &r_rec) {
                      auto &&lhs = l_rec->data + cols_.offset;
                      auto &&rhs = r_rec->data + cols_.offset;
                      return is_desc_
                                 ? compare(lhs, rhs, cols_.len, cols_.type) > 0
                                 : compare(lhs, rhs, cols_.len, cols_.type) < 0;
                  });

        current_tuple_ = std::move(records_.front());
        records_.pop_front();
    }

    void nextTuple() override {
        if (records_.empty()) {
            return;
        }

        current_tuple_ = std::move(records_.front());
        records_.pop_front();
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(current_tuple_);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const { return current_tuple_ == nullptr; }

    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }

    size_t tupleLen() const override { return prev_->tupleLen(); }
};
