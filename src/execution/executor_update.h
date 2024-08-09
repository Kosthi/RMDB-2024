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
#include <utility>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;
    std::vector<std::vector<ColMeta>::iterator> set_cols_;
    bool is_set_index_key_;

public:
    UpdateExecutor(SmManager *sm_manager, std::string tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, bool is_set_index_key, bool is_index_scan, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        set_clauses_ = std::move(set_clauses);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        conds_ = std::move(conds);
        // 已经通过扫描算子找到了满足谓词条件的 rids
        // 不如同时把 records 也给我
        rids_ = std::move(rids);
        is_set_index_key_ = is_set_index_key;
        context_ = context;

        for (auto &set: set_clauses_) {
            set_cols_.emplace_back(tab_.get_col(set.lhs.col_name));
        }

        // X 锁
        if (!is_index_scan && !rids_.empty() && context_ != nullptr) {
            context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
            for (auto &[ix_name, index_meta]: tab_.indexes) {
                auto predicate_manager = PredicateManager(index_meta);
                auto gap = Gap(predicate_manager.getIndexConds());
                context_->lock_mgr_->lock_exclusive_on_gap(context_->txn_, index_meta, gap, fh_->GetFd());
            }
        }
    }

    // 这里 next 只会被调用一次
    std::unique_ptr<RmRecord> Next() override {
        for (auto &rid: rids_) {
            auto old_record = fh_->get_record(rid, context_);
            auto updated_record = std::make_unique<RmRecord>(*old_record);

            for (size_t i = 0; i < set_clauses_.size(); ++i) {
                auto &col_meta = set_cols_[i];
                if (set_clauses_[i].is_incr) {
                    add(updated_record->data + col_meta->offset, set_clauses_[i].rhs.raw->data, col_meta->type);
                } else {
                    memcpy(updated_record->data + col_meta->offset, set_clauses_[i].rhs.raw->data, col_meta->len);
                }
            }

            if (is_set_index_key_) {
                auto **old_keys = new char *[tab_.indexes.size()];
                auto **new_keys = new char *[tab_.indexes.size()];
                auto **ihs = new IxIndexHandle *[tab_.indexes.size()];

                int i = 0;
                // 索引查重
                for (auto &[ix_name, index]: tab_.indexes) {
                    ihs[i] = sm_manager_->ihs_[ix_name].get();
                    old_keys[i] = new char[index.col_tot_len];
                    new_keys[i] = new char[index.col_tot_len];
                    for (auto &[index_offset, col_meta]: index.cols) {
                        memcpy(old_keys[i] + index_offset, old_record->data + col_meta.offset, col_meta.len);
                        memcpy(new_keys[i] + index_offset,
                               updated_record->data + col_meta.offset, col_meta.len);
                    }
                    if (!ihs[i]->is_unique(new_keys[i], _abstract_rid, context_->txn_) && _abstract_rid != rid) {
                        for (int j = 0; j <= i; ++j) {
                            delete []old_keys[j];
                            delete []new_keys[j];
                        }
                        delete []old_keys;
                        delete []new_keys;
                        delete []ihs;
                        throw NonUniqueIndexError("", {ix_name});
                    }
                    ++i;
                }

                // 再检查是否有间隙锁
                // TPCC 测试中 update 不会涉及键的变化，在 index scan 算子加了写间隙锁后就不用再检查了
                for (auto &[index_name, index]: tab_.indexes) {
                    RmRecord rm_record(index.col_tot_len);
                    for (auto &[index_offset, col_meta]: index.cols) {
                        memcpy(rm_record.data + index_offset, updated_record->data + col_meta.offset, col_meta.len);
                    }
                    context_->lock_mgr_->isSafeInGap(context_->txn_, index, rm_record, fh_->GetFd());
                }

                // Unique Index -> Insert into index
                for (int j = 0; j < i; ++j) {
                    // TODO 如果不涉及索引建的变化，直接原地更新记录，不需要维护索引，避免B+树分裂重组的不必要耗时
                    ihs[j]->delete_entry(old_keys[j], context_->txn_);
                    ihs[j]->insert_entry(new_keys[j], rid, context_->txn_);
                    delete []old_keys[j];
                    delete []new_keys[j];
                }

                // 插入完成，释放内存
                delete []old_keys;
                delete []new_keys;
                delete []ihs;
            }

            // 再检查是否有间隙锁
            // TPCC 测试中 update 不会涉及键的变化，在 index scan 算子加了写间隙锁后就不用再检查了
            // for (auto &[index_name, index]: tab_.indexes) {
            //     RmRecord rm_record(index.col_tot_len);
            //     for (auto &[index_offset, col_meta]: index.cols) {
            //         memcpy(rm_record.data + index_offset, updated_record->data + col_meta.offset, col_meta.len);
            //     }
            //     context_->lock_mgr_->isSafeInGap(context_->txn_, index, rm_record);
            // }

#ifdef ENABLE_LOGGING
            auto *update_log_record = new UpdateLogRecord(context_->txn_->get_transaction_id(), *old_record,
                                                          *updated_record, rid, tab_name_);
            update_log_record->prev_lsn_ = context_->txn_->get_prev_lsn();
            context_->txn_->set_prev_lsn(context_->log_mgr_->add_log_to_buffer(update_log_record));
            auto &&page = fh_->fetch_page_handle(rid.page_no).page;
            page->set_page_lsn(context_->txn_->get_prev_lsn());
            sm_manager_->get_bpm()->unpin_page(page->get_page_id(), true);
            delete update_log_record;
#endif

            fh_->update_record(rid, updated_record->data, context_);

            // 防止 double throw
            // 写入事务写集
            auto *write_record = new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, *old_record, *updated_record, is_set_index_key_);
            context_->txn_->append_write_record(write_record);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
