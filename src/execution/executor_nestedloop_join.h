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

class NestedLoopJoinExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> left_; // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_; // 右儿子节点（需要join的表）
    size_t len_; // join后获得的每条记录的长度
    std::vector<ColMeta> cols_; // join后获得的记录的字段
    std::vector<Condition> fed_conds_; // join条件
    std::unique_ptr<RmRecord> rm_record_;
    std::unique_ptr<RmRecord> lhs_rec_;
    bool is_right_empty_;
    bool sort_mode_{false};
    int last_cmp_;

public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds): left_(std::move(left)), right_(std::move(right)),
                                                          fed_conds_(std::move(conds)) {
        if (left_->getType() == "SortExecutor" && right_->getType() == "SortExecutor") {
            sort_mode_ = true;
        }
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col: right_cols) {
            col.offset += left_->tupleLen();
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        is_right_empty_ = false;
    }

    void beginTuple() override {
        left_->beginTuple();
        // 左表为空时直接返回
        if (left_->is_end()) {
            return;
        }
        right_->beginTuple();
        // 右表为空时直接返回
        if (right_->is_end()) {
            is_right_empty_ = true;
            return;
        }

        do {
            lhs_rec_ = left_->Next();
            while (!right_->is_end()) {
                auto &&rhs_rec = right_->Next();
                if (cmp_conds(lhs_rec_.get(), rhs_rec.get(), fed_conds_, cols_)) {
                    rm_record_ = std::make_unique<RmRecord>(len_);
                    // 拷贝左右元组
                    memcpy(rm_record_->data, lhs_rec_->data, left_->tupleLen());
                    memcpy(rm_record_->data + left_->tupleLen(), rhs_rec->data, right_->tupleLen());
                    return;
                }
                if (sort_mode_ && last_cmp_ < 0) {
                    break;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            right_->beginTuple();
        } while (!left_->is_end());
    }

    void nextTuple() override {
        if (left_->is_end()) {
            return;
        }
        right_->nextTuple();

        do {
            while (!right_->is_end()) {
                auto &&rhs_rec = right_->Next();
                if (cmp_conds(lhs_rec_.get(), rhs_rec.get(), fed_conds_, cols_)) {
                    rm_record_ = std::make_unique<RmRecord>(len_);
                    // 拷贝左右元组
                    memcpy(rm_record_->data, lhs_rec_->data, left_->tupleLen());
                    memcpy(rm_record_->data + left_->tupleLen(), rhs_rec->data, right_->tupleLen());
                    return;
                }
                if (sort_mode_ && last_cmp_ < 0) {
                    break;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            if (left_->is_end()) {
                break;
            }
            lhs_rec_ = left_->Next();
            right_->beginTuple();
        } while (!left_->is_end());
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(rm_record_);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const { return left_->is_end() || is_right_empty_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    size_t tupleLen() const override { return len_; }

    static inline int compare(const char *a, const char *b, int col_len, ColType col_type) {
        switch (col_type) {
            case TYPE_INT: {
                const int ai = *reinterpret_cast<const int *>(a);
                const int bi = *reinterpret_cast<const int *>(b);
                return (ai > bi) - (ai < bi);
            }
            case TYPE_FLOAT: {
                const float af = *reinterpret_cast<const float *>(a);
                const float bf = *reinterpret_cast<const float *>(b);
                return (af > bf) - (af < bf);
            }
            case TYPE_STRING:
                return memcmp(a, b, col_len);
            default:
                throw InternalError("Unexpected data type！");
        }
    }

    // 判断是否满足单个谓词条件
    bool cmp_cond(const RmRecord *lhs_rec, const RmRecord *rhs_rec, const Condition &cond,
                  const std::vector<ColMeta> &rec_cols) {
        // 左边必然为列值
        const auto &lhs_col_meta = get_col(rec_cols, cond.lhs_col);
        const char *lhs_data = lhs_rec->data + lhs_col_meta->offset;
        const char *rhs_data;
        ColType rhs_type;

        // 提取左值与右值的数据和类型
        // 常值
        if (cond.is_rhs_val) {
            rhs_type = cond.rhs_val.type;
            rhs_data = cond.rhs_val.raw->data;
        } else {
            // 列值
            const auto &rhs_col_meta = get_col(rec_cols, cond.rhs_col);
            rhs_type = rhs_col_meta->type;
            rhs_data = rhs_rec->data + rhs_col_meta->offset - left_->tupleLen(); // 记得减去左边的 tuplelen
        }

        if (lhs_col_meta->type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_col_meta->type), coltype2str(rhs_type));
        }

        last_cmp_ = compare(lhs_data, rhs_data, lhs_col_meta->len, lhs_col_meta->type);
        switch (cond.op) {
            case OP_EQ: return last_cmp_ == 0;
            case OP_NE: return last_cmp_ != 0;
            case OP_LT: return last_cmp_ < 0;
            case OP_GT: return last_cmp_ > 0;
            case OP_LE: return last_cmp_ <= 0;
            case OP_GE: return last_cmp_ >= 0;
            default:
                throw InternalError("Unexpected op type！");
        }
    }

    bool cmp_conds(const RmRecord *lhs_rec, const RmRecord *rhs_rec, const std::vector<Condition> &conds,
                   const std::vector<ColMeta> &rec_cols) {
        return std::all_of(conds.begin(), conds.end(), [&](const Condition &cond) {
            return cmp_cond(lhs_rec, rhs_rec, cond, rec_cols);
        });
    }
};
