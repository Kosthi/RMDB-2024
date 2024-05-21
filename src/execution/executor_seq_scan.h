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
    }

    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_);
        for (; !scan_->is_end(); scan_->next()) {
            rid_ = scan_->rid();
            rm_record_ = std::move(fh_->get_record(rid_, context_));
            if (cmp_conds(rm_record_.get(), conds_, cols_)) {
                break;
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

    bool is_end() const { return scan_->is_end(); }

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
