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

class DeleteExecutor : public AbstractExecutor {
private:
    TabMeta tab_; // 表的元数据
    std::vector<Condition> conds_; // delete的条件
    RmFileHandle *fh_; // 表的数据文件句柄
    std::vector<Rid> rids_; // 需要删除的记录的位置
    std::string tab_name_; // 表名称
    SmManager *sm_manager_;
    bool is_index_scan_{false};

public:
    DeleteExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context, bool is_index_scan = false): sm_manager_(sm_manager),
        tab_name_(std::move(tab_name)),
        conds_(std::move(conds)),
        rids_(std::move(rids)), is_index_scan_(is_index_scan) {
        tab_ = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        context_ = context;

        // S_IX 锁
        if (!rids_.empty() && !is_index_scan_ && context_ != nullptr) {
            // context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
            context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
        }
    }

    // 只执行一次
    std::unique_ptr<RmRecord> Next() override {
        for (auto &rid: rids_) {
            auto &&rec = fh_->get_record(rid, context_);

            auto *delete_log_record = new DeleteLogRecord(context_->txn_->get_transaction_id(), *rec, rid, tab_name_);
            delete_log_record->prev_lsn_ = context_->txn_->get_prev_lsn();
            context_->txn_->set_prev_lsn(context_->log_mgr_->add_log_to_buffer(delete_log_record));
            auto &&page = fh_->fetch_page_handle(rid.page_no).page;
            page->set_page_lsn(context_->txn_->get_prev_lsn());
            sm_manager_->get_bpm()->unpin_page(page->get_page_id(), true);
            delete delete_log_record;

            // 写入事务写集
            auto *write_record = new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, *rec);
            context_->txn_->append_write_record(write_record);

            // 如果有索引，则必然是唯一索引
            for (auto &[index_name, index]: tab_.indexes) {
                auto &&ih = sm_manager_->ihs_.at(index_name).get();
                char *key = new char[index.col_tot_len];
                for (auto &[index_offset, col_meta]: index.cols) {
                    memcpy(key + index_offset, rec->data + col_meta.offset, col_meta.len);
                }
                ih->delete_entry(key, context_->txn_);
                delete []key;
            }
            fh_->delete_record(rid, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
