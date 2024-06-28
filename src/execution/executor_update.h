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

public:
    UpdateExecutor(SmManager *sm_manager, std::string tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        set_clauses_ = std::move(set_clauses);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        conds_ = std::move(conds);
        // 已经通过扫描算子找到了满足谓词条件的 rids
        // 不如同时把 records 也给我
        rids_ = std::move(rids);
        context_ = context;

        // S_IX 锁
        if (!rids_.empty() && context_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
            context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
        }
    }

    // 这里 next 只会被调用一次
    std::unique_ptr<RmRecord> Next() override {
        for (auto &rid: rids_) {
            auto &&updated_record = fh_->get_record(rid, context_);
            auto old_record = std::make_unique<RmRecord>(*updated_record);

            for (auto &set: set_clauses_) {
                auto &&col_meta = tab_.get_col(set.lhs.col_name);
                memcpy(updated_record->data + col_meta->offset, set.rhs.raw->data, col_meta->len);
            }

            // 先检查 key 是否是 unique
            for (auto &[index_name, index]: tab_.indexes) {
                auto &&ih = sm_manager_->ihs_.at(index_name).get();
                // TODO 优化 放到容器中
                char *key = new char[index.col_tot_len];
                for (auto &[index_offset, col_meta]: index.cols) {
                    memcpy(key + index_offset, updated_record->data + col_meta.offset, col_meta.len);
                }
                // TODO 如果 update a = 5 where a = 5，应该在优化器阶段优化掉
                Rid unique_rid{};
                if (!ih->is_unique(key, unique_rid, context_->txn_) && rid != unique_rid) {
                    delete []key;
                    throw NonUniqueIndexError("", {index_name});
                }
                delete []key;
            }

            auto *update_log_record = new UpdateLogRecord(context_->txn_->get_transaction_id(), *old_record,
                                                          *updated_record, rid, tab_name_);
            update_log_record->prev_lsn_ = context_->txn_->get_prev_lsn();
            context_->txn_->set_prev_lsn(context_->log_mgr_->add_log_to_buffer(update_log_record));
            auto &&page = fh_->fetch_page_handle(rid.page_no).page;
            page->set_page_lsn(context_->txn_->get_prev_lsn());
            sm_manager_->get_bpm()->unpin_page(page->get_page_id(), true);
            delete update_log_record;

            // 写入事务写集
            auto *write_record = new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, *old_record, *updated_record);
            context_->txn_->append_write_record(write_record);

            // Unique Index -> Insert into index
            for (auto &[index_name, index]: tab_.indexes) {
                auto ih = sm_manager_->ihs_.at(index_name).get();
                char *old_key = new char[index.col_tot_len];
                char *new_key = new char[index.col_tot_len];
                for (auto &[index_offset, col_meta]: index.cols) {
                    memcpy(old_key + index_offset, old_record->data + col_meta.offset, col_meta.len);
                    memcpy(new_key + index_offset, updated_record->data + col_meta.offset, col_meta.len);
                }
                ih->delete_entry(old_key, context_->txn_);
                ih->insert_entry(new_key, rid, context_->txn_);
                delete []old_key;
                delete []new_key;
            }

            fh_->update_record(rid, updated_record->data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
