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

class InsertExecutor : public AbstractExecutor {
private:
    TabMeta tab_; // 表的元数据
    std::vector<Value> values_; // 需要插入的数据
    RmFileHandle *fh_; // 表的数据文件句柄
    std::string tab_name_; // 表名称
    Rid rid_; // 插入的位置，由于系统默认插入时不指定位置，因此当前rid_在插入后才赋值
    SmManager *sm_manager_;

public:
    InsertExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Value> values, Context *context) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        if (values.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;

        // IX 锁
        if (context_ != nullptr) {
            context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
        }
    };

    std::unique_ptr<RmRecord> Next() override {
        // Make record buffer
        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            if (col.type == TYPE_FLOAT && val.type == TYPE_INT) {
                val.set_float(static_cast<float>(val.int_val));
            } else if (col.type != val.type) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            }
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }

        // 先检查 key 是否是 unique
        for (auto &[index_name, index]: tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(index_name).get();
            int offset = 0;
            // TODO 优化 放到容器中
            char *key = new char[index.col_tot_len];
            for (size_t i = 0; i < index.col_num; ++i) {
                memcpy(key + offset, rec.data + index.cols[i].offset, index.cols[i].len);
                offset += index.cols[i].len;
            }
            Rid unique_rid{};
            if (!ih->is_unique(key, unique_rid, context_->txn_)) {
                delete []key;
                throw NonUniqueIndexError("", {index_name});
            }
            delete []key;
        }

        // Insert into record file
        rid_ = fh_->insert_record(rec.data, context_);

        // auto *insert_log_record = new InsertLogRecord(context_->txn_->get_transaction_id(), rec, rid_, tab_name_);
        // insert_log_record->prev_lsn_ = context_->txn_->get_prev_lsn();
        // context_->txn_->set_prev_lsn(context_->log_mgr_->add_log_to_buffer(insert_log_record));
        // auto &&page = fh_->fetch_page_handle(rid_.page_no).page;
        // page->set_page_lsn(context_->txn_->get_prev_lsn());
        // sm_manager_->get_bpm()->unpin_page(page->get_page_id(), true);

        // 写入事务写集
        // may std::unique_ptr 优化，避免拷贝多次记录
        auto *write_record = new WriteRecord(WType::INSERT_TUPLE, rid_, rec, tab_name_);
        context_->txn_->append_write_record(write_record);

        // Unique Index -> Insert into index
        for (auto &[index_name, index]: tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(index_name).get();
            char *key = new char[index.col_tot_len];
            int offset = 0;
            for (size_t i = 0; i < index.col_num; ++i) {
                memcpy(key + offset, rec.data + index.cols[i].offset, index.cols[i].len);
                offset += index.cols[i].len;
            }
            ih->insert_entry(key, rid_, context_->txn_);
            delete []key;
        }
        return nullptr;
    }

    Rid &rid() override { return rid_; }
};
