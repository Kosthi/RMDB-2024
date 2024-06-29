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

class SeqScanExecutor : public AbstractExecutor {
private:
    std::string tab_name_; // 表的名称
    std::vector<Condition> conds_; // scan的条件
    RmFileHandle *fh_; // 表的数据文件句柄
    std::vector<ColMeta> cols_; // scan后生成的记录的字段
    size_t len_; // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_; // 同conds_，两个字段相同
    Rid rid_;
    std::unique_ptr<RecScan> scan_; // table_iterator
    SmManager *sm_manager_;
    std::unique_ptr<RmRecord> rm_record_;
    std::vector<bool> is_need_scan_; // 是否需要扫表（非子查询）
    bool is_sub_query_empty_;

public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;
        context_ = context;
        fed_conds_ = conds_;
        is_sub_query_empty_ = false;
        // S 锁
        if (context_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        }
    }

    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_);
        for (; !scan_->is_end(); scan_->next()) {
            rid_ = scan_->rid();
            rm_record_ = std::move(fh_->get_record(rid_, context_));
            if (cmp_conds(rm_record_.get(), conds_, cols_)) {
                break;
            }
            if (is_sub_query_empty_) {
                return;
            }
        }
    }

    void nextTuple() override {
        if (scan_->is_end()) {
            return;
        }
        for (scan_->next(); !scan_->is_end(); scan_->next()) {
            rid_ = scan_->rid();
            rm_record_ = fh_->get_record(rid_, context_);
            if (cmp_conds(rm_record_.get(), conds_, cols_)) {
                break;
            }
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(rm_record_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const { return is_sub_query_empty_ || scan_->is_end(); }

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
    bool cmp_cond(const RmRecord *rec, const Condition &cond, const std::vector<ColMeta> &rec_cols) {
        const auto &lhs_col_meta = get_col(rec_cols, cond.lhs_col);
        const char *lhs_data = rec->data + lhs_col_meta->offset;
        char *rhs_data;
        ColType rhs_type;
        // 全局record 防止作为临时变量离开作用域自动析构，char* 指针指向错误的地址
        std::unique_ptr<RmRecord> record;
        // 提取左值与右值的数据和类型
        // 常值
        if (cond.is_rhs_val) {
            rhs_type = cond.rhs_val.type;
            rhs_data = cond.rhs_val.raw->data;
        } else if (cond.is_sub_query) {
            // 查的是值列表
            if (!cond.rhs_value_list.empty()) {
                // in 谓词
                if (cond.op == OP_IN) {
                    // 前面已经强制转换和检查类型匹配过了，这里不需要
                    for (auto &value: cond.rhs_value_list) {
                        rhs_data = value.raw->data;
                        if (compare(lhs_data, rhs_data, lhs_col_meta->len, value.type) == 0) {
                            return true;
                        }
                    }
                    return false;
                }
                // 比较谓词
                assert(cond.rhs_value_list.size() == 1);
                auto &value = cond.rhs_value_list[0];
                int cmp = compare(lhs_data, value.raw->data, lhs_col_meta->len, value.type);
                switch (cond.op) {
                    case OP_EQ: return cmp == 0;
                    case OP_NE: return cmp != 0;
                    case OP_LT: return cmp < 0;
                    case OP_GT: return cmp > 0;
                    case OP_LE: return cmp <= 0;
                    case OP_GE: return cmp >= 0;
                    default:
                        throw InternalError("Unexpected op type！");
                }
            }
            // 查的是子算子
            cond.prev->beginTuple();
            // 如果直接结束则直接返回空表
            if (cond.prev->is_end()) {
                throw InternalError("Empty sub query!");
                is_sub_query_empty_ = true;
                return false;
            }

            // where id <= (select count(*) from grade);
            // where id <= (select id from grade where id = 1); // 返回单列单行
            ColMeta rhs_col_meta;
            if (cond.sub_query->agg_types[0] == AGG_COUNT && cond.sub_query->cols[0].tab_name.empty() && cond.sub_query
                ->cols[0].col_name.empty()) {
                rhs_type = TYPE_INT;
            } else {
                // ！子查询右边的列值类型应该由子算子决定
                rhs_col_meta = cond.prev->cols()[0];
                // where id > (select count(id) from grade);
                if (cond.sub_query->agg_types[0] == AGG_COUNT) {
                    rhs_type = TYPE_INT;
                } else {
                    rhs_type = rhs_col_meta.type;
                }
            }

            // 处理 in 谓词，扫描算子直到找到一个完全相等的
            if (cond.op == OP_IN) {
                for (; !cond.prev->is_end(); cond.prev->nextTuple()) {
                    record = cond.prev->Next();
                    rhs_data = record->data;
                    if (lhs_col_meta->type == TYPE_FLOAT && rhs_type == TYPE_INT) {
                        rhs_type = TYPE_FLOAT;
                        const float a = *reinterpret_cast<const int *>(rhs_data);
                        memcpy(rhs_data, &a, sizeof(float));
                    }
                    if (compare(lhs_data, rhs_data, lhs_col_meta->len, rhs_type) == 0) {
                        return true;
                    }
                }
                return false;
            }

            // 聚合或列值只能有一行
            record = cond.prev->Next();
            cond.prev->nextTuple();
            if (!cond.prev->is_end()) {
                throw InternalError("Subquery returns more than 1 row!");
            }
            rhs_data = record->data;
            if (lhs_col_meta->type == TYPE_FLOAT && rhs_type == TYPE_INT) {
                rhs_type = TYPE_FLOAT;
                const float a = *reinterpret_cast<const int *>(rhs_data);
                memcpy(rhs_data, &a, sizeof(float));
            }
        } else {
            // 列值
            const auto &rhs_col_meta = get_col(rec_cols, cond.rhs_col);
            rhs_type = rhs_col_meta->type;
            rhs_data = rec->data + rhs_col_meta->offset;
        }

        if (lhs_col_meta->type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_col_meta->type), coltype2str(rhs_type));
        }

        int cmp = compare(lhs_data, rhs_data, lhs_col_meta->len, rhs_type);
        switch (cond.op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
            default:
                throw InternalError("Unexpected op type！");
        }
    }

    bool cmp_conds(const RmRecord *rec, const std::vector<Condition> &conds, const std::vector<ColMeta> &rec_cols) {
        return std::all_of(conds.begin(), conds.end(), [&](const Condition &cond) {
            return cmp_cond(rec, cond, rec_cols);
        });
    }
};
