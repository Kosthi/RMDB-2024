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
#include "predicate_manager.h"

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
        // X 锁
        // if (context_ != nullptr) {
        //     context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
        // }
    }

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


        // 把索引键缓存
        auto **keys = new char *[tab_.indexes.size()];
        auto **ihs = new IxIndexHandle *[tab_.indexes.size()];

        int i = 0;
        // 先索引查重
        for (auto &[ix_name, index]: tab_.indexes) {
            ihs[i] = sm_manager_->ihs_[ix_name].get();
            keys[i] = new char[index.col_tot_len];
            for (auto &[index_offset, col_meta]: index.cols) {
                memcpy(keys[i] + index_offset,
                       rec.data + col_meta.offset, col_meta.len);
            }
            if (!ihs[i]->is_unique(keys[i], rid_, context_->txn_)) {
                for (int j = 0; j <= i; ++j) {
                    delete []keys[j];
                }
                delete []keys;
                delete []ihs;
                throw NonUniqueIndexError("", {ix_name});
            }
            ++i;
        }

        // 再检查是否有间隙锁
        for (auto &[index_name, index]: tab_.indexes) {
            RmRecord rm_record(index.col_tot_len);
            for (auto &[index_offset, col_meta]: index.cols) {
                memcpy(rm_record.data + index_offset, rec.data + col_meta.offset, col_meta.len);
            }
            context_->lock_mgr_->isSafeInGap(context_->txn_, index, rm_record);
        }

        // 再检查是否有间隙锁冲突，这里间隙锁退化成了行锁
        // for (auto &[index_name, index_meta]: tab_.indexes) {
        //     auto predicate_manager = PredicateManager(index_meta);
        //
        //     // TODO 支持更多谓词的解析 > >
        //     // 手动写个 cond index_col = val
        //     std::vector<Condition> conds;
        //     for (auto &[index_offset, col_meta]: index_meta.cols) {
        //         Value v;
        //         v.type = col_meta.type;
        //         v.raw = std::make_shared<RmRecord>(rec.data + col_meta.offset, col_meta.len);
        //         conds.emplace_back();
        //         conds.back().op = OP_EQ;
        //         conds.back().lhs_col = {"", col_meta.name};
        //         conds.back().rhs_val = std::move(v);
        //     }
        //     for (auto it = conds.begin(); it != conds.end();) {
        //         if (predicate_manager.addPredicate(it->lhs_col.col_name, *it)) {
        //             it = conds.erase(it);
        //             continue;
        //         }
        //         assert(0);
        //         ++it;
        //     }
        //     auto gap = Gap(predicate_manager.getIndexConds());
        //     // RmRecord rm_record(index.col_tot_len);
        //     // for (auto &[index_offset, col_meta]: index.cols) {
        //     //     memcpy(rm_record.data + index_offset, rec.data + col_meta.offset, col_meta.len);
        //     // }
        //     context_->lock_mgr_->lock_exclusive_on_gap(context_->txn_, index_meta, gap, fh_->GetFd());
        // }

        // Insert into record file
        rid_ = fh_->insert_record(rec.data, context_);

#ifdef ENABLE_LOGGING
        auto *insert_log_record = new InsertLogRecord(context_->txn_->get_transaction_id(), rec, rid_, tab_name_);
        insert_log_record->prev_lsn_ = context_->txn_->get_prev_lsn();
        context_->txn_->set_prev_lsn(context_->log_mgr_->add_log_to_buffer(insert_log_record));
        auto &&page = fh_->fetch_page_handle(rid_.page_no).page;
        page->set_page_lsn(context_->txn_->get_prev_lsn());
        sm_manager_->get_bpm()->unpin_page(page->get_page_id(), true);
        delete insert_log_record;
#endif

        // 插入完成，释放内存
        for (int j = 0; j < i; ++j) {
            ihs[j]->insert_entry(keys[j], rid_, context_->txn_);
            delete []keys[j];
        }
        delete []keys;
        delete []ihs;

        // 防止 double throw
        // 写入事务写集
        // may std::unique_ptr 优化，避免拷贝多次记录
        auto *write_record = new WriteRecord(WType::INSERT_TUPLE, rid_, rec, tab_name_);
        context_->txn_->append_write_record(write_record);
        return nullptr;
    }

    Rid &rid() override { return rid_; }
};
