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

// 解析列的谓词信息 first >, second <
class PredicateManager {
public:
    PredicateManager() = default;

    // TODO 暂时仅支持索引列解析
    explicit PredicateManager(IndexMeta &index_meta) {
        for (int i = 0; i < index_meta.cols.size(); ++i) {
            predicates_.emplace(index_meta.cols[i].second.name, i);
            index_conds_.emplace_back(CondOp{.offset = index_meta.cols[i].first},
                                      CondOp{.offset = index_meta.cols[i].first});
        }
    }

    bool addPredicate(const std::string &column, Condition &cond) {
        // 非索引字段
        if (predicates_.count(column) == 0) {
            return false;
        }
        // 左边
        if (cond.op == OP_GT || cond.op == OP_GE || cond.op == OP_EQ) {
            insertLeft(column, cond);
        }
        // 右边
        if (cond.op == OP_LT || cond.op == OP_LE || cond.op == OP_EQ) {
            insertRight(column, cond);
        }
        return true;
    }

    void insertLeft(const std::string &column, Condition &cond) {
        index_conds_[predicates_[column]].first.op = cond.op;
        index_conds_[predicates_[column]].first.rhs_val = cond.rhs_val;
    }

    void insertRight(const std::string &column, Condition &cond) {
        index_conds_[predicates_[column]].second.op = cond.op;
        index_conds_[predicates_[column]].second.rhs_val = std::move(cond.rhs_val);
    }

    CondOp getLeft(const std::string &column) {
        return index_conds_[predicates_[column]].first;
    }

    CondOp getRight(const std::string &column) {
        return index_conds_[predicates_[column]].second;
    }

    bool cmpIndexConds(const RmRecord &rec) {
        return cmpIndexLeftConds(rec) && cmpIndexRightConds(rec);
    }

    bool cmpIndexLeftConds(const RmRecord &rec) {
        for (auto &[cond, _]: index_conds_) {
            if (cond.op != OP_INVALID && !cmpIndexCond(rec, cond)) {
                return false;
            }
        }
        return true;
    }

    bool cmpIndexRightConds(const RmRecord &rec) {
        for (auto &[_, cond]: index_conds_) {
            if (cond.op != OP_INVALID && !cmpIndexCond(rec, cond)) {
                return false;
            }
        }
        return true;
    }

    static bool cmpIndexCond(const RmRecord &rec, const CondOp &cond) {
        int cmp = compare(rec.data + cond.offset, cond.rhs_val.raw->data, cond.rhs_val.raw->size, cond.rhs_val.type);
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

    // 左边谓词第一个不为等号的
    std::tuple<CompOp, int> getLeftLastTuple(char *&key) {
        int last_idx = 0; // 第一个范围查询位置

        CompOp op;
        for (auto &[cond, _]: index_conds_) {
            op = cond.op;
            if (op == OP_INVALID) {
                break;
            }
            memcpy(key + cond.offset, cond.rhs_val.raw->data, cond.rhs_val.raw->size);
            if (op != OP_EQ) {
                break;
            }
            ++last_idx;
        }

        return {op, last_idx};
    }

    std::tuple<CompOp, int> getRightLastTuple(char *&key) {
        int last_idx = 0; // 第一个范围查询位置

        CompOp op;
        for (auto &[_, cond]: index_conds_) {
            op = cond.op;
            if (op == OP_INVALID) {
                break;
            }
            memcpy(key + cond.offset, cond.rhs_val.raw->data, cond.rhs_val.raw->size);
            if (op != OP_EQ) {
                break;
            }
            ++last_idx;
        }

        return {op, last_idx};
    }

    std::vector<std::pair<CondOp, CondOp> > &getIndexConds() {
        return index_conds_;
    }

private:
    std::unordered_map<std::string, int> predicates_;
    std::vector<std::pair<CondOp, CondOp> > index_conds_;
};

class IndexScanExecutor : public AbstractExecutor {
private:
    std::string tab_name_; // 表名称
    TabMeta tab_; // 表的元数据
    std::vector<Condition> conds_; // 扫描条件
    RmFileHandle *fh_; // 表的数据文件句柄
    std::vector<ColMeta> cols_; // 需要读取的字段
    size_t len_; // 选取出来的一条记录的长度
    std::vector<std::string> index_col_names_; // index scan涉及到的索引包含的字段
    IndexMeta index_meta_; // index scan涉及到的索引元数据
    Rid rid_;
    std::unique_ptr<IxScan> scan_;
    SmManager *sm_manager_;
    std::unique_ptr<RmRecord> rm_record_;
    std::deque<std::unique_ptr<RmRecord> > records_;
    // 实际二进制文件名
    std::string filename_;
    // 排序结果实际读取文件，二进制
    std::fstream outfile_;
    bool is_end_;
    size_t id_;
    bool mergesort_;
    constexpr static int int_min_ = INT32_MIN;
    constexpr static int int_max_ = INT32_MAX;
    constexpr static float float_min_ = FLT_MIN;
    constexpr static float float_max_ = FLT_MAX;

    // 满足索引算子多次调用，非归并
    bool already_begin_{false};
    Iid lower_;
    Iid upper_;
    IxIndexHandle *ih_;

    // false 为共享间隙锁，true 为互斥间隙锁
    bool gap_mode_;
    PredicateManager predicate_manager_;

    bool is_empty_btree_{false};

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
                      Context *context, bool gap_mode = false) : sm_manager_(sm_manager),
                                                                 tab_name_(std::move(tab_name)),
                                                                 conds_(std::move(conds)),
                                                                 index_col_names_(std::move(index_col_names)),
                                                                 gap_mode_(gap_mode) {
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
        id_ = generateID();
        filename_ = "sorted_results_index_" + std::to_string(id_) + ".txt";
        mergesort_ = !conds_[0].is_rhs_val && conds_.size() == 1;

        const auto &&index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        ih_ = sm_manager_->ihs_[index_name].get();

        predicate_manager_ = PredicateManager(index_meta_);

        // TODO 支持更多谓词的解析 > >
        for (auto it = conds_.begin(); it != conds_.end();) {
            if (it->lhs_col.tab_name == tab_name_ && it->op != OP_NE && it->is_rhs_val) {
                if (predicate_manager_.addPredicate(it->lhs_col.col_name, *it)) {
                    it = conds_.erase(it);
                    continue;
                }
            }
            ++it;
        }

        auto gap = Gap(predicate_manager_.getIndexConds());
        if (gap_mode_) {
            context_->lock_mgr_->lock_exclusive_on_gap(context_->txn_, index_meta_, gap, fh_->GetFd());
        } else {
            context_->lock_mgr_->lock_shared_on_gap(context_->txn_, index_meta_, gap, fh_->GetFd());
        }

        // S 锁
        // if (context_ != nullptr) {
        //     context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        // }
    }

    void beginTuple() override {
        if (is_empty_btree_) {
            return;
        }
        if (already_begin_ && !mergesort_) {
            is_end_ = false;
            scan_ = std::make_unique<IxScan>(ih_, lower_, upper_, sm_manager_->get_bpm());
            while (!scan_->is_end()) {
                // 不回表
                // 全是等号或最后一个谓词是比较，不需要再扫索引
                if (predicate_manager_.cmpIndexConds(scan_->get_key())) {
                    // 回表，查不在索引里的谓词
                    rid_ = scan_->rid();
                    rm_record_ = fh_->get_record(rid_, context_);
                    if (conds_.empty() || cmp_conds(rm_record_.get(), conds_, cols_)) {
                        return;
                    }
                }
                scan_->next();
            }
            is_end_ = true;
            return;
        }

        // 空树
        if (ih_->is_empty()) {
            is_empty_btree_ = is_end_ = true;
            return;
        }

        lower_ = ih_->leaf_begin(), upper_ = ih_->leaf_end();
        // 如果 '列 op 列'，走 sortmerge
        // TODO 行多外部排序慢，但是为了通过归并连接测试
        if (mergesort_) {
            // 适用嵌套连接走索引的情况
            // struct stat st;
            // 当前算子排序过了，已经存在排序结果文件
            if (already_begin_) {
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
            scan_ = std::make_unique<IxScan>(ih_, lower_, upper_, sm_manager_->get_bpm());
            write_sorted_results();

            already_begin_ = true;

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

        char *left_key = new char[index_meta_.col_tot_len];
        char *right_key = new char[index_meta_.col_tot_len];

        // 找出下界 [
        auto last_left_tuple = predicate_manager_.getLeftLastTuple(left_key);
        auto last_right_tuple = predicate_manager_.getRightLastTuple(right_key);

        auto last_left_op = std::get<0>(last_left_tuple);
        auto last_right_op = std::get<0>(last_right_tuple);

        auto last_idx = std::get<1>(last_left_tuple); // 第一个范围查询位置

        assert(last_idx == std::get<1>(last_right_tuple));

        // index(a, b, c) where a = 1, b = 1 等值查询
        if (last_left_op == OP_INVALID && last_right_op == OP_INVALID) {
            set_remaining_all_min(last_idx, left_key);
            lower_ = ih_->lower_bound(left_key);
            // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
            set_remaining_all_max(last_idx, right_key);
            upper_ = ih_->upper_bound(right_key);
        } else {
            switch (last_left_op) {
                // 交给查上界的处理
                case OP_INVALID: {
                    break;
                }
                // 全部都是等值查询
                case OP_EQ: {
                    // where p_id = 0, name = 'bztyhnmj';
                    // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
                    assert(last_idx == index_meta_.cols.size());
                    // set_remaining_all_min(offset, last_idx, key);
                    lower_ = ih_->lower_bound(left_key);
                    // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
                    // set_remaining_all_max(offset, last_idx, key);
                    upper_ = ih_->upper_bound(right_key);

                    // 1.最简单的情况，唯一索引等值锁定存在的数据：加行锁index(a, b, c) a = 1, b = 1, c = 1
                    // 加间隙锁
                    // 1.1 a = 1
                    // 1.2 a = 1, b = 1

                    break;
                }
                case OP_GT: {
                    // where name > 'bztyhnmj';                      last_idx = 0, + 1
                    // where name > 'bztyhnmj' and id = 1;           last_idx = 0, + 1
                    // where p_id = 3, name > 'bztyhnmj';            last_idx = 1, + 1
                    // where p_id = 3, name > 'bztyhnmj' and id = 1; last_idx = 1, + 1
                    // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
                    // TODO may error?
                    set_remaining_all_max(last_idx + 1, left_key);
                    lower_ = ih_->upper_bound(left_key);
                    // 如果前面有等号需要重新更新上下界
                    // where w_id = 0 and name > 'bztyhnmj';
                    if (last_right_op == OP_INVALID) {
                        if (last_idx > 0) {
                            // 把后面的范围查询清 0 找上限
                            // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
                            set_remaining_all_max(last_idx, left_key);
                            upper_ = ih_->upper_bound(left_key);
                        }
                    }
                    break;
                }
                case OP_GE: {
                    // where name >= 'bztyhnmj';                      last_idx = 0, + 1
                    // where name >= 'bztyhnmj' and id = 1;           last_idx = 0, + 1
                    // where p_id = 3, name >= 'bztyhnmj';            last_idx = 1, + 1
                    // where p_id = 3, name >= 'bztyhnmj' and id = 1; last_idx = 1, + 1
                    // 如果前面有等号需要重新更新上下界
                    // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
                    set_remaining_all_min(last_idx + 1, left_key);
                    lower_ = ih_->lower_bound(left_key);
                    // where w_id = 0 and name >= 'bztyhnmj';
                    if (last_right_op == OP_INVALID) {
                        if (last_idx > 0) {
                            // 把后面的范围查询置最大 找上限
                            // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
                            set_remaining_all_max(last_idx, left_key);
                            upper_ = ih_->upper_bound(left_key);
                        }
                    }
                    break;
                }
                default:
                    throw InternalError("Unexpected op type！");
            }

            // 找出上界 )
            switch (last_right_op) {
                // 在前面查下界时已经确定
                case OP_INVALID: {
                    break;
                }
                // 全部都是等值查询
                case OP_EQ: {
                    // where p_id = 0, name = 'bztyhnmj';
                    // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
                    // set_remaining_all_min(offset, last_idx, right_key);
                    assert(last_idx == index_meta_.cols.size());

                    lower_ = ih_->lower_bound(right_key);
                    // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
                    // set_remaining_all_max(offset, last_idx, right_key);
                    upper_ = ih_->upper_bound(right_key);

                    // 1.最简单的情况，唯一索引等值锁定存在的数据：加行锁index(a, b, c) a = 1, b = 1, c = 1
                    // 加间隙锁
                    // 1.1 a = 1
                    // 1.2 a = 1, b = 1

                    break;
                }
                case OP_LT: {
                    // where name < 'bztyhnmj';                      last_idx = 0, + 1
                    // where name < 'bztyhnmj' and id = 1;           last_idx = 0, + 1
                    // where p_id = 3, name < 'bztyhnmj';            last_idx = 1, + 1
                    // where p_id = 3, name < 'bztyhnmj' and id = 1; last_idx = 1, + 1
                    // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
                    set_remaining_all_min(last_idx + 1, right_key);
                    upper_ = ih_->lower_bound(right_key);
                    // 如果前面有等号需要重新更新上下界
                    // where w_id = 0 and name < 'bztyhnmj';
                    if (last_left_op == OP_INVALID) {
                        if (last_idx > 0) {
                            // 把后面的范围查询清 0 找下限
                            // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
                            set_remaining_all_min(last_idx, right_key);
                            lower_ = ih_->lower_bound(right_key);
                        }
                    }
                    break;
                }
                case OP_LE: {
                    // where name <= 'bztyhnmj';                      last_idx = 0, + 1
                    // where name <= 'bztyhnmj' and id = 1;           last_idx = 0, + 1
                    // where p_id = 3, name <= 'bztyhnmj';            last_idx = 1, + 1
                    // where p_id = 3, name <= 'bztyhnmj' and id = 1; last_idx = 1, + 1
                    // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
                    // TODO error?
                    set_remaining_all_max(last_idx + 1, right_key);
                    upper_ = ih_->upper_bound(right_key);
                    // 如果前面有等号需要重新更新上下界
                    // where w_id = 0 and name <= 'bztyhnmj';
                    if (last_left_op == OP_INVALID) {
                        if (last_idx > 0) {
                            // 把后面的范围查询清 0 找下限
                            // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
                            set_remaining_all_min(last_idx, right_key);
                            lower_ = ih_->lower_bound(right_key);
                        }
                    }
                    break;
                }
                default:
                    throw InternalError("Unexpected op type！");
            }
        }

        // switch (last_cond.op) {
        //     // 全部都是等值查询
        //     case OP_EQ: {
        //         // where p_id = 0, name = 'bztyhnmj';
        //         // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
        //         set_remaining_all_min(offset, last_idx, key);
        //         lower_ = ih_->lower_bound(key);
        //         // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
        //         set_remaining_all_max(offset, last_idx, key);
        //         upper_ = ih_->upper_bound(key);
        //
        //         // 1.最简单的情况，唯一索引等值锁定存在的数据：加行锁index(a, b, c) a = 1, b = 1, c = 1
        //         // 加间隙锁
        //         // 1.1 a = 1
        //         // 1.2 a = 1, b = 1
        //
        //         break;
        //     }
        //     case OP_GE: {
        //         // where name >= 'bztyhnmj';                      last_idx = 0, + 1
        //         // where name >= 'bztyhnmj' and id = 1;           last_idx = 0, + 1
        //         // where p_id = 3, name >= 'bztyhnmj';            last_idx = 1, + 1
        //         // where p_id = 3, name >= 'bztyhnmj' and id = 1; last_idx = 1, + 1
        //         // 如果前面有等号需要重新更新上下界
        //         // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
        //         set_remaining_all_min(offset, last_idx + 1, key);
        //         lower_ = ih_->lower_bound(key);
        //         // where w_id = 0 and name >= 'bztyhnmj';
        //         if (last_idx > 0) {
        //             // 把后面的范围查询置最大 找上限
        //             // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
        //             set_remaining_all_max(equal_offset, last_idx, key);
        //             upper_ = ih_->upper_bound(key);
        //         }
        //         break;
        //     }
        //     case OP_LE: {
        //         // where name <= 'bztyhnmj';                      last_idx = 0, + 1
        //         // where name <= 'bztyhnmj' and id = 1;           last_idx = 0, + 1
        //         // where p_id = 3, name <= 'bztyhnmj';            last_idx = 1, + 1
        //         // where p_id = 3, name <= 'bztyhnmj' and id = 1; last_idx = 1, + 1
        //         // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
        //         set_remaining_all_max(offset, last_idx + 1, key);
        //         upper_ = ih_->upper_bound(key);
        //         // 如果前面有等号需要重新更新上下界
        //         // where w_id = 0 and name <= 'bztyhnmj';
        //         if (last_idx > 0) {
        //             // 把后面的范围查询清 0 找下限
        //             // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
        //             set_remaining_all_min(equal_offset, last_idx, key);
        //             lower_ = ih_->lower_bound(key);
        //         }
        //         break;
        //     }
        //     case OP_GT: {
        //         // where name > 'bztyhnmj';                      last_idx = 0, + 1
        //         // where name > 'bztyhnmj' and id = 1;           last_idx = 0, + 1
        //         // where p_id = 3, name > 'bztyhnmj';            last_idx = 1, + 1
        //         // where p_id = 3, name > 'bztyhnmj' and id = 1; last_idx = 1, + 1
        //         // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
        //         set_remaining_all_max(offset, last_idx + 1, key);
        //         lower_ = ih_->upper_bound(key);
        //         // 如果前面有等号需要重新更新上下界
        //         // where w_id = 0 and name > 'bztyhnmj';
        //         if (last_idx > 0) {
        //             // 把后面的范围查询清 0 找上限
        //             // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
        //             set_remaining_all_max(equal_offset, last_idx, key);
        //             upper_ = ih_->upper_bound(key);
        //         }
        //         break;
        //     }
        //     case OP_LT: {
        //         // where name < 'bztyhnmj';                      last_idx = 0, + 1
        //         // where name < 'bztyhnmj' and id = 1;           last_idx = 0, + 1
        //         // where p_id = 3, name < 'bztyhnmj';            last_idx = 1, + 1
        //         // where p_id = 3, name < 'bztyhnmj' and id = 1; last_idx = 1, + 1
        //         // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
        //         set_remaining_all_min(offset, last_idx + 1, key);
        //         upper_ = ih_->lower_bound(key);
        //         // 如果前面有等号需要重新更新上下界
        //         // where w_id = 0 and name < 'bztyhnmj';
        //         if (last_idx > 0) {
        //             // 把后面的范围查询清 0 找下限
        //             // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
        //             set_remaining_all_min(equal_offset, last_idx, key);
        //             lower_ = ih_->lower_bound(key);
        //         }
        //         break;
        //     }
        //     case OP_NE:
        //         break;
        //     default:
        //         throw InternalError("Unexpected op type！");
        // }

        // 释放内存
        delete []left_key;
        delete []right_key;

        // switch (last_cond.op) {
        //     // 全部都是等值查询
        //     case OP_EQ: {
        //         // where p_id = 0, name = 'bztyhnmj';
        //         // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
        //         set_remaining_all_min(offset, last_idx, key);
        //         lower_ = ih_->lower_bound(key);
        //         // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
        //         set_remaining_all_max(offset, last_idx, key);
        //         upper_ = ih_->upper_bound(key);
        //
        //         // 1.最简单的情况，唯一索引等值锁定存在的数据：加行锁index(a, b, c) a = 1, b = 1, c = 1
        //         // 加间隙锁
        //         // 1.1 a = 1
        //         // 1.2 a = 1, b = 1
        //
        //         break;
        //     }
        //     case OP_GT: {
        //         // where name > 'bztyhnmj';                      last_idx = 0, + 1
        //         // where name > 'bztyhnmj' and id = 1;           last_idx = 0, + 1
        //         // where p_id = 3, name > 'bztyhnmj';            last_idx = 1, + 1
        //         // where p_id = 3, name > 'bztyhnmj' and id = 1; last_idx = 1, + 1
        //         // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
        //         set_remaining_all_max(offset, last_idx + 1, key);
        //         lower_ = ih_->upper_bound(key);
        //         // 如果前面有等号需要重新更新上下界
        //         // where w_id = 0 and name > 'bztyhnmj';
        //         if (last_idx > 0) {
        //             // 把后面的范围查询清 0 找上限
        //             // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
        //             set_remaining_all_max(equal_offset, last_idx, key);
        //             upper_ = ih_->upper_bound(key);
        //         }
        //
        //         // 左边取第一个> 右边取第一个小于
        //         // a = 1, b > 1, c > 1  // 用upper_bound找然后-1 lower 找111 找到为开，找不到去前面一个为开，upper不动
        //         // a = 1, b >= 1, c > 1 // lower 找111 找到为开，找不到去前面一个为开，upper不动
        //
        //         // 扫索引，不回表，直到匹配
        //         for (; !scan_->is_end(); scan_->next()) {
        //             // TODO 优化，不一定全匹配
        //             if (predicate_manager_.cmpIndexLeftConds(ih_->get_key(scan_->iid()))) {
        //                 // 下界为
        //                 scan_->prev_iid();
        //             }
        //         }
        //         break;
        //     }
        //     case OP_GE: {
        //         // where name >= 'bztyhnmj';                      last_idx = 0, + 1
        //         // where name >= 'bztyhnmj' and id = 1;           last_idx = 0, + 1
        //         // where p_id = 3, name >= 'bztyhnmj';            last_idx = 1, + 1
        //         // where p_id = 3, name >= 'bztyhnmj' and id = 1; last_idx = 1, + 1
        //         // 如果前面有等号需要重新更新上下界
        //         // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
        //         set_remaining_all_min(offset, last_idx + 1, key);
        //         lower_ = ih_->lower_bound(key);
        //         // where w_id = 0 and name >= 'bztyhnmj';
        //         if (last_idx > 0) {
        //             // 把后面的范围查询置最大 找上限
        //             // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
        //             set_remaining_all_max(equal_offset, last_idx, key);
        //             upper_ = ih_->upper_bound(key);
        //         }
        //
        //         // 左边取第一个> 右边取第一个小于
        //         // a = 1, b > 1, c >= 1 // lower 找111 找到为开，找不到去前面一个为开，upper不动
        //         // a = 1, b >= 1, c >= 1 // lower 找111 找到为开，找不到去前面一个为开，upper不动
        //
        //         break;
        //     }
        //     case OP_LE: {
        //         // where name <= 'bztyhnmj';                      last_idx = 0, + 1
        //         // where name <= 'bztyhnmj' and id = 1;           last_idx = 0, + 1
        //         // where p_id = 3, name <= 'bztyhnmj';            last_idx = 1, + 1
        //         // where p_id = 3, name <= 'bztyhnmj' and id = 1; last_idx = 1, + 1
        //         // 设置成最大值，需要根据类型设置，不能直接0xff，int 为 -1
        //         set_remaining_all_max(offset, last_idx + 1, key);
        //         upper_ = ih_->upper_bound(key);
        //         // 如果前面有等号需要重新更新上下界
        //         // where w_id = 0 and name <= 'bztyhnmj';
        //         if (last_idx > 0) {
        //             // 把后面的范围查询清 0 找下限
        //             // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
        //             set_remaining_all_min(equal_offset, last_idx, key);
        //             lower_ = ih_->lower_bound(key);
        //         }
        //         break;
        //     }
        //     case OP_LT: {
        //         // where name < 'bztyhnmj';                      last_idx = 0, + 1
        //         // where name < 'bztyhnmj' and id = 1;           last_idx = 0, + 1
        //         // where p_id = 3, name < 'bztyhnmj';            last_idx = 1, + 1
        //         // where p_id = 3, name < 'bztyhnmj' and id = 1; last_idx = 1, + 1
        //         // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
        //         set_remaining_all_min(offset, last_idx + 1, key);
        //         upper_ = ih_->lower_bound(key);
        //         // 如果前面有等号需要重新更新上下界
        //         // where w_id = 0 and name < 'bztyhnmj';
        //         if (last_idx > 0) {
        //             // 把后面的范围查询清 0 找下限
        //             // 设置成最小值，需要根据类型设置，不能直接0，int 会有负值
        //             set_remaining_all_min(equal_offset, last_idx, key);
        //             lower_ = ih_->lower_bound(key);
        //         }
        //         break;
        //     }
        //     case OP_NE:
        //         break;
        //     default:
        //         throw InternalError("Unexpected op type！");
        // }
        //
        // RmRecord lower_rec(index_meta_.col_tot_len);
        // RmRecord upper_rec(index_meta_.col_tot_len);
        //
        // set_remaining_all_min(0, 0, lower_rec.data);
        // set_remaining_all_max(0, 0, upper_rec.data);
        //
        // // 实现最小粒度的间隙，查询条件即为加锁范围
        // for (auto &cond : conds_) {
        //     assert(cond.lhs_col.tab_name == tab_name_);
        //     auto it = index_meta_.cols_map.find(cond.lhs_col.col_name);
        //     if (it != index_meta_.cols_map.end() && cond.op != OP_NE ) {
        //         // TODO 先不涉及 同种谓词出现两次以上 id > 1 and id > 10
        //         // index(a, b, c) a > 1 and b >= 2 and a <= 10
        //         if (cond.op == OP_GT || cond.op == OP_GE || cond.op == OP_EQ) {
        //             memcpy(lower_rec.data + it->second.first, cond.rhs_val.raw->data, it->second.second.len);
        //         }
        //         if (cond.op == OP_LT || cond.op == OP_LE || cond.op == OP_EQ) {
        //             memcpy(upper_rec.data + it->second.first, cond.rhs_val.raw->data, it->second.second.len);
        //         }
        //     }
        // }
        //
        // if (gap_mode_) {
        //     context_->lock_mgr_->lock_exclusive_on_gap(context_->txn_, index_meta_, lower_rec, upper_rec, fh_->GetFd());
        // } else {
        //     context_->lock_mgr_->lock_shared_on_gap(context_->txn_, index_meta_, lower_rec, upper_rec, fh_->GetFd());
        // }
        //
        // lower_ = ih_->lower_bound(lower_rec.data);
        // upper_ = ih_->upper_bound(upper_rec.data);
        //
        // auto lower_actual = fh_->get_record(ih_->get_rid(lower_), context_);
        //
        // scan_ = std::make_unique<IxScan>(ih_, lower_, upper_, sm_manager_->get_bpm());
        //
        // // 1.最简单的情况，唯一索引等值锁定存在的数据：加行锁index(a, b, c) a = 1, b = 1, c = 1
        // if (ix_compare(lower_rec.data, upper_rec.data, index_meta_) == 0) {
        //     // 记录存在，加行锁
        //     if (!scan_->is_end()) {
        //         if (gap_mode_) {
        //             context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid_, fh_->GetFd());
        //         } else {
        //             context_->lock_mgr_->lock_shared_on_record(context_->txn_, rid_, fh_->GetFd());
        //         }
        //     } else {
        //         // 记录不存在，在查找条件所在间隙加间隙锁
        //         // 确定上界
        //         upper_rec = ih_->get_key(scan_->iid());
        //         lower_rec = ih_->get_key(scan_->prev_iid());
        //         if (gap_mode_) {
        //             context_->lock_mgr_->lock_exclusive_on_gap(context_->txn_, index_meta_, lower_rec, upper_rec, fh_->GetFd());
        //         } else {
        //             context_->lock_mgr_->lock_shared_on_gap(context_->txn_, index_meta_, lower_rec, upper_rec, fh_->GetFd());
        //         }
        //     }
        // } else {
        //     // index(a, b, c) 存在范围
        //     // 1.1 a = 1
        //     // 1.2 a = 1, b = 1
        //     // 全部是等号
        //     // 直接加间隙锁
        //     if (gap_mode_) {
        //         context_->lock_mgr_->lock_exclusive_on_gap(context_->txn_, index_meta_, lower_rec, upper_rec, fh_->GetFd());
        //     } else {
        //         context_->lock_mgr_->lock_shared_on_gap(context_->txn_, index_meta_, lower_rec, upper_rec, fh_->GetFd());
        //     }
        //
        //     // 2.1.1 a = 1, b = 1，c > 1 (1, +INF)
        //     // 2.1.2 a = 1, b = 1，c >= 1 [1], (1, +INF)
        //     // 2.2   a = 1, b > 1, c > 1 // lower 找111 找到为开，找不到去前面一个为开，upper不动
        //     // 2.3   a = 1, b > 1, c > 1, b < 5 lower 找111 找到为开，找不到去前面一个为开，上界用lower找第一个大于等于5的
        //     // 2.4   a = 1, b > 1, b < 5, c > 1, c < 3
        //     auto lower_fact = ih_->get_key(scan_->iid());
        //
        //     lower_fact = ih_->get_key(scan_->prev_iid(ih_->upper_bound(lower_rec.data)));
        //
        //
        //
        //     if (ix_compare(lower_fact.data, lower_rec.data, index_meta_) == 0) {
        //
        //     }
        //
        //     ih_->lower_bound(lower_rec.data);
        // }

        // 确定间隙锁真正的上下界
        // while (!scan_->is_end()) {
        //     rid_ = scan_->rid();
        //     // 唯一索引等值查询
        //     rm_record_ = fh_->get_record(rid_, context_);
        //     if (cmp_conds(rm_record_.get(), fed_conds_, cols_)) {
        //         break;
        //     }
        // }
        //
        // // 第一个满足的即为下界
        // auto lower_rid = ih_->get_rid(scan_->prev_iid());
        //
        // // 谓词的比较值作为下界
        // if (lower_rid.page_no == -1 || lower_rid.slot_no == -1) {
        //
        // }
        //
        // lower_rec = fh_->get_record(lower_rid, context_);

        scan_ = std::make_unique<IxScan>(ih_, lower_, upper_, sm_manager_->get_bpm());
        already_begin_ = true;

        // where a > 1, c < 1
        while (!scan_->is_end()) {
            // 不回表
            // 全是等号或最后一个谓词是比较，不需要再扫索引
            if (predicate_manager_.cmpIndexConds(scan_->get_key())) {
                // 回表，查不在索引里的谓词
                rid_ = scan_->rid();
                rm_record_ = fh_->get_record(rid_, context_);
                if (conds_.empty() || cmp_conds(rm_record_.get(), conds_, cols_)) {
                    return;
                }
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
        if (mergesort_) {
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
            // 不回表
            // 全是等号或最后一个谓词是比较，不需要再扫索引
            // TODO 待优化
            if (predicate_manager_.cmpIndexConds(scan_->get_key())) {
                // 回表，查不在索引里的谓词
                rid_ = scan_->rid();
                rm_record_ = fh_->get_record(rid_, context_);
                if (conds_.empty() || cmp_conds(rm_record_.get(), conds_, cols_)) {
                    return;
                }
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
    void set_remaining_all_max(int last_idx, char *&key) {
        // 设置成最大值
        for (auto i = last_idx; i < index_meta_.cols.size(); ++i) {
            auto &[index_offset, col] = index_meta_.cols[i];
            if (col.type == TYPE_INT) {
                memcpy(key + index_offset, &int_max_, sizeof(int));
            } else if (col.type == TYPE_FLOAT) {
                memcpy(key + index_offset, &float_max_, sizeof(float));
            } else if (col.type == TYPE_STRING) {
                memset(key + index_offset, 0xff, col.len);
            } else {
                throw InternalError("Unexpected data type！");
            }
        }
    }

    // 根据不同的列值类型设置不同的最小值
    // int   类型范围 int_min_ ~ int_max_
    // float 类型范围 float_min_ ~ float_max_
    // char  类型范围 0 ~ 255
    void set_remaining_all_min(int last_idx, char *&key) {
        for (auto i = last_idx; i < index_meta_.cols.size(); ++i) {
            auto &[index_offset, col] = index_meta_.cols[i];
            if (col.type == TYPE_INT) {
                memcpy(key + index_offset, &int_min_, sizeof(int));
            } else if (col.type == TYPE_FLOAT) {
                memcpy(key + index_offset, &float_min_, sizeof(float));
            } else if (col.type == TYPE_STRING) {
                memset(key + index_offset, 0, col.len);
            } else {
                throw InternalError("Unexpected data type！");
            }
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
            // 查的是子算子，在算子生成阶段已经检查了合法性
            cond.prev->beginTuple();

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

    std::string getType() { return "IndexScanExecutor"; }
};
