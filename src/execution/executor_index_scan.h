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

#define MAX_RECORD_SIZE 81920

#include <float.h>
#include <limits.h>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
private:
    std::string tab_name_; // 表名称
    TabMeta tab_; // 表的元数据
    std::vector<Condition> conds_; // 扫描条件
    RmFileHandle *fh_; // 表的数据文件句柄
    std::vector<ColMeta> cols_; // 需要读取的字段
    size_t len_; // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_; // 扫描条件，和conds_字段相同
    std::vector<std::string> index_col_names_; // index scan涉及到的索引包含的字段
    IndexMeta index_meta_; // index scan涉及到的索引元数据
    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    SmManager *sm_manager_;
    std::unique_ptr<RmRecord> rm_record_;
    std::deque<std::unique_ptr<RmRecord> > records_;
    // 实际二进制文件名
    std::string filename_;
    // 排序结果实际读取文件，二进制
    std::fstream outfile_;
    bool is_end_;
    size_t id_;
    constexpr static int int_min_ = INT32_MIN;
    constexpr static int int_max_ = INT32_MAX;
    constexpr static float float_min_ = FLT_MIN;
    constexpr static float float_max_ = FLT_MAX;

    static std::size_t generateID() {
        static size_t current_id = 0;
        return ++current_id;
    }

    // for index scan
    void write_sorted_results() {
        // 以期望格式写入 sorted_results.txt
        auto out_expected_file = std::fstream("sorted_results.txt", std::ios::out | std::ios::app);
        outfile_.open(filename_, std::ios::out);

        // 打印右表头
        out_expected_file << "|";
        for (auto &col_meta: cols_) {
            out_expected_file << " " << col_meta.name << " |";
        }
        out_expected_file << "\n";

        // 右表先开始
        for (; !scan_->is_end(); scan_->next()) {
            // 打印记录
            rm_record_ = fh_->get_record(scan_->rid(), context_);
            // 写入文件中
            outfile_.write(rm_record_->data, rm_record_->size);

            std::vector<std::string> columns;
            columns.reserve(cols_.size());

            for (auto &col: cols_) {
                std::string col_str;
                char *rec_buf = rm_record_->data + col.offset;
                if (col.type == TYPE_INT) {
                    col_str = std::to_string(*(int *) rec_buf);
                } else if (col.type == TYPE_FLOAT) {
                    col_str = std::to_string(*(float *) rec_buf);
                } else if (col.type == TYPE_STRING) {
                    col_str = std::string((char *) rec_buf, col.len);
                    col_str.resize(strlen(col_str.c_str()));
                }
                columns.emplace_back(col_str);
            }

            // 打印记录
            out_expected_file << "|";
            for (auto &col: columns) {
                out_expected_file << " " << col << " |";
            }
            out_expected_file << "\n";
        }

        outfile_.close();
        out_expected_file.close();
    }

public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                      std::vector<std::string> index_col_names,
                      Context *context) : sm_manager_(sm_manager), tab_name_(std::move(tab_name)),
                                          conds_(std::move(conds)),
                                          index_col_names_(std::move(index_col_names)) {
        context_ = context;
        tab_ = sm_manager_->db_.get_table(tab_name_);
        // index_no_ = index_no;
        index_meta_ = tab_.get_index_meta(index_col_names_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond: conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
        std::reverse(fed_conds_.begin(), fed_conds_.end());
        id_ = generateID();
        filename_ = "sorted_results_index_" + std::to_string(id_) + ".txt";
    }

    void beginTuple() override {
        const auto &&index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        const auto &&ih = sm_manager_->ihs_[index_name].get();

        Iid lower = ih->leaf_begin(), upper = ih->leaf_end();
        // sortmerge
        if (!conds_[0].is_rhs_val) {
            // 适用嵌套连接走索引的情况
            struct stat st;
            // 当前算子排序过了，已经存在排序结果文件
            if (stat(filename_.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                // 可能是算子结束进来的，也有可能是到一部分重新开始了
                if (!is_end_) {
                    outfile_.close();
                }

                // 记得一定要重置，否则读到文件尾后算子会一直保持结束状态
                is_end_ = false;

                outfile_.open(filename_, std::ios::in);
                if (!outfile_.is_open()) {
                    std::stringstream s;
                    s << "Failed to open file: " << std::strerror(errno);
                    throw InternalError(s.str());
                }

                // 缓存记录
                do {
                    rm_record_ = std::make_unique<RmRecord>(len_);
                    outfile_.read(rm_record_->data, rm_record_->size);
                    if (outfile_.gcount() == 0) {
                        break;
                    }
                    records_.emplace_back(std::move(rm_record_));
                } while (records_.size() < MAX_RECORD_SIZE);

                if (!records_.empty()) {
                    rm_record_ = std::move(records_.front());
                    records_.pop_front();
                } else {
                    is_end_ = true;
                    rm_record_ = nullptr;
                    outfile_.close();
                }
                return;
            }

            is_end_ = false;
            scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());
            write_sorted_results();

            outfile_.open(filename_, std::ios::in);

            // 缓存记录
            do {
                rm_record_ = std::make_unique<RmRecord>(len_);
                outfile_.read(rm_record_->data, rm_record_->size);
                if (outfile_.gcount() == 0) {
                    break;
                }
                records_.emplace_back(std::move(rm_record_));
            } while (records_.size() < MAX_RECORD_SIZE);

            if (!records_.empty()) {
                rm_record_ = std::move(records_.front());
                records_.pop_front();
            } else {
                is_end_ = true;
                rm_record_ = nullptr;
                outfile_.close();
            }
            return;
        }

        is_end_ = false;

        char *key = new char[index_meta_.col_tot_len];

        int offset = 0;
        int equal_offset = 0; // 如果前面有等值查询，记录等值查询列值的长度
        int last_idx = 0; // 第一个范围查询位置

        for (auto &cond: conds_) {
            // 右边一定是数值
            memcpy(key + offset, cond.rhs_val.raw->data, cond.rhs_val.raw->size);
            offset += cond.rhs_val.raw->size;
            // 移除已经在索引中的的谓词
            fed_conds_.pop_back();
            // 非等值查询
            if (cond.op != OP_EQ) {
                break;
            }
            equal_offset = offset;
            ++last_idx;
        }

        const auto &last_cond = conds_[last_idx == conds_.size() ? last_idx - 1 : last_idx];

        switch (last_cond.op) {
            // 全部都是等值查询
            case OP_EQ: {
                // where p_id = 0, name = 'bztyhnmj';
                // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
                set_remaining_all_min(offset, last_idx, key);
                lower = ih->lower_bound(key);
                // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
                set_remaining_all_max(offset, last_idx, key);
                upper = ih->upper_bound(key);
                break;
            }
            case OP_GE: {
                // where name >= 'bztyhnmj';                      last_idx = 0, + 1
                // where name >= 'bztyhnmj' and id = 1;           last_idx = 0, + 1
                // where p_id = 3, name >= 'bztyhnmj';            last_idx = 1, + 1
                // where p_id = 3, name >= 'bztyhnmj' and id = 1; last_idx = 1, + 1
                // 如果前面有等号需要重新更新上下界
                // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
                set_remaining_all_min(offset, last_idx + 1, key);
                lower = ih->lower_bound(key);
                // where w_id = 0 and name >= 'bztyhnmj';
                if (last_idx > 0) {
                    // 把后面的范围查询置最大 找上限
                    // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
                    set_remaining_all_max(equal_offset, last_idx, key);
                    upper = ih->upper_bound(key);
                }
                break;
            }
            case OP_LE: {
                // where name <= 'bztyhnmj';                      last_idx = 0, + 1
                // where name <= 'bztyhnmj' and id = 1;           last_idx = 0, + 1
                // where p_id = 3, name <= 'bztyhnmj';            last_idx = 1, + 1
                // where p_id = 3, name <= 'bztyhnmj' and id = 1; last_idx = 1, + 1
                // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
                set_remaining_all_max(offset, last_idx + 1, key);
                upper = ih->upper_bound(key);
                // 如果前面有等号需要重新更新上下界
                // where w_id = 0 and name <= 'bztyhnmj';
                if (last_idx > 0) {
                    // 把后面的范围查询清 0 找下限
                    // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
                    set_remaining_all_min(equal_offset, last_idx, key);
                    lower = ih->lower_bound(key);
                }
                break;
            }
            case OP_GT: {
                // where name > 'bztyhnmj';                      last_idx = 0, + 1
                // where name > 'bztyhnmj' and id = 1;           last_idx = 0, + 1
                // where p_id = 3, name > 'bztyhnmj';            last_idx = 1, + 1
                // where p_id = 3, name > 'bztyhnmj' and id = 1; last_idx = 1, + 1
                // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
                set_remaining_all_max(offset, last_idx + 1, key);
                lower = ih->upper_bound(key);
                // 如果前面有等号需要重新更新上下界
                // where w_id = 0 and name > 'bztyhnmj';
                if (last_idx > 0) {
                    // 把后面的范围查询清 0 找上限
                    // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
                    set_remaining_all_max(equal_offset, last_idx, key);
                    upper = ih->upper_bound(key);
                }
                break;
            }
            case OP_LT: {
                // where name < 'bztyhnmj';                      last_idx = 0, + 1
                // where name < 'bztyhnmj' and id = 1;           last_idx = 0, + 1
                // where p_id = 3, name < 'bztyhnmj';            last_idx = 1, + 1
                // where p_id = 3, name < 'bztyhnmj' and id = 1; last_idx = 1, + 1
                // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
                set_remaining_all_min(offset, last_idx + 1, key);
                upper = ih->lower_bound(key);
                // 如果前面有等号需要重新更新上下界
                // where w_id = 0 and name < 'bztyhnmj';
                if (last_idx > 0) {
                    // 把后面的范围查询清 0 找下限
                    // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
                    set_remaining_all_min(equal_offset, last_idx, key);
                    lower = ih->lower_bound(key);
                }
                break;
            }
            case OP_NE:
                break;
            default:
                throw InternalError("Unexpected op type！");
        }

        // 释放内存
        delete []key;

        scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            rm_record_ = fh_->get_record(rid_, context_);
            if (cmp_conds(rm_record_.get(), fed_conds_, cols_)) {
                return;
            }
            scan_->next();
        }
        is_end_ = true;
    }

    void nextTuple() override {
        if (!records_.empty()) {
            rm_record_ = std::move(records_.front());
            records_.pop_front();
            return;
        }
        // sortmerge
        if (!conds_[0].is_rhs_val) {
            // 缓存记录
            do {
                rm_record_ = std::make_unique<RmRecord>(len_);
                outfile_.read(rm_record_->data, rm_record_->size);
                if (outfile_.gcount() == 0) {
                    break;
                }
                records_.emplace_back(std::move(rm_record_));
            } while (records_.size() < MAX_RECORD_SIZE);

            if (!records_.empty()) {
                rm_record_ = std::move(records_.front());
                records_.pop_front();
            } else {
                is_end_ = true;
                rm_record_ = nullptr;
                outfile_.close();
            }
            return;
        }
        if (scan_->is_end()) {
            is_end_ = true;
            return;
        }
        for (scan_->next(); !scan_->is_end(); scan_->next()) {
            rid_ = scan_->rid();
            rm_record_ = fh_->get_record(rid_, context_);
            if (cmp_conds(rm_record_.get(), fed_conds_, cols_)) {
                return;
            }
        }
        is_end_ = true;
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(rm_record_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const { return is_end_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    size_t tupleLen() const override { return len_; }

    // 根据不同的列值类型设置不同的最大值
    // int   类型范围 int_min_ ~ int_max_
    // float 类型范围 float_min_ ~ float_max_
    // char  类型范围 0 ~ 255
    void set_remaining_all_max(int offset, int last_idx, char *&key) {
        // 设置成最大值
        for (auto i = last_idx; i < index_meta_.cols.size(); ++i) {
            auto &col = index_meta_.cols[i];
            if (col.type == TYPE_INT) {
                memcpy(key + offset, &int_max_, sizeof(int));
            } else if (col.type == TYPE_FLOAT) {
                memcpy(key + offset, &float_max_, sizeof(float));
            } else if (col.type == TYPE_STRING) {
                memset(key + offset, 0xff, col.len);
            } else {
                throw InternalError("Unexpected data type！");
            }
            offset += col.len;
        }
    }

    // 根据不同的列值类型设置不同的最小值
    // int   类型范围 int_min_ ~ int_max_
    // float 类型范围 float_min_ ~ float_max_
    // char  类型范围 0 ~ 255
    void set_remaining_all_min(int offset, int last_idx, char *&key) {
        for (auto i = last_idx; i < index_meta_.cols.size(); ++i) {
            auto &col = index_meta_.cols[i];
            if (col.type == TYPE_INT) {
                memcpy(key + offset, &int_min_, sizeof(int));
            } else if (col.type == TYPE_FLOAT) {
                memcpy(key + offset, &float_min_, sizeof(float));
            } else if (col.type == TYPE_STRING) {
                memset(key + offset, 0, col.len);
            } else {
                throw InternalError("Unexpected data type！");
            }
            offset += col.len;
        }
    }

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

    std::string getType() { return "IndexScanExecutor"; }
};
