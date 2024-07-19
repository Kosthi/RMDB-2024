/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include "record/rm_manager.h"

#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction *TransactionManager::begin(Transaction *txn, LogManager *log_manager) {
    // Todo:
    // 1. 判断传入事务参数是否为空指针
    // 2. 如果为空指针，创建新事务
    // 3. 把开始事务加入到全局事务表中
    // 4. 返回当前事务指针
    std::lock_guard lock(latch_);

    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }
    txn->set_start_ts(next_timestamp_++);
    txn_map.emplace(txn->get_transaction_id(), txn);

    // MVCC support
    txn->read_ts_.store(last_commit_ts_);
    running_txns_.AddTxn(txn->read_ts_);

#ifdef ENABLE_LOGGING
    auto *begin_log_record = new BeginLogRecord(txn->get_transaction_id());
    begin_log_record->prev_lsn_ = txn->get_prev_lsn();
    // TODO 日志管理
    txn->set_prev_lsn(log_manager->add_log_to_buffer(begin_log_record));
    delete begin_log_record;
#endif
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction *txn, LogManager *log_manager) {
    // Todo:
    // 1. 如果存在未提交的写操作，提交所有的写操作
    // 2. 释放所有锁
    // 3. 释放事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    std::lock_guard lock(latch_);

    // 释放写集指针
    for (auto &it: *txn->get_write_set()) {
        delete it;
    }

    // 释放所有锁
    auto &&lock_set = txn->get_lock_set();
    for (auto &it: *lock_set) {
        lock_manager_->unlock(txn, it);
    }
    lock_set->clear();

    // 获取提交时间戳 注意还不能增加 因为还不稳定
    auto &&commit_ts = last_commit_ts_.load() + 1;
    // 为所有元组时间戳设置为提交时间戳
    for (auto &[table_oid, rids] : txn->GetWriteSets()) {
        auto &&table_heap = catalog_->GetTable(table_oid)->table_;
        for (auto &rid : rids) {
            auto &&tuple_meta = table_heap->GetTupleMeta(rid);
            tuple_meta.ts_ = commit_ts;
            table_heap->UpdateTupleMeta(tuple_meta, rid);
        }
    }

    // 事务设置为已提交状态
    txn->state_ = TransactionState::COMMITTED;

    // 更新last_commit_ts_
    // 因为读取时间戳是最新提交时间戳，新的提交时间戳应该+1 提交时间戳一定是最新的
    last_commit_ts_.fetch_add(1);
    txn->commit_ts_.store(last_commit_ts_);
    running_txns_.UpdateCommitTs(txn->commit_ts_);
    running_txns_.RemoveTxn(txn->read_ts_);

#ifdef ENABLE_LOGGING
    auto *commit_log_record = new CommitLogRecord(txn->get_transaction_id());
    commit_log_record->prev_lsn_ = txn->get_prev_lsn();
    // TODO 日志管理
    txn->set_prev_lsn(log_manager->add_log_to_buffer(commit_log_record));
    log_manager->flush_log_to_disk();
    delete commit_log_record;
#endif
    txn->set_state(TransactionState::COMMITTED);
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    // Todo:
    // 1. 回滚所有写操作
    // 2. 释放所有锁
    // 3. 清空事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    std::lock_guard lock(latch_);

    auto &&write_set = txn->get_write_set();
    auto *context = new Context(lock_manager_, log_manager, txn);
    // 从最后一个向前回滚
    for (auto &&it = write_set->rbegin(); it != write_set->rend(); ++it) {
        auto &write_record = *it;
        auto &table_name = write_record->GetTableName();
        auto &table_meta = sm_manager_->db_.get_table(table_name);
        auto &fh = sm_manager_->fhs_[table_name];
        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                // 删除记录
                auto &rid = write_record->GetRid();
                auto &record = write_record->GetRecord();
                fh->delete_record(rid, context);
                // 删除索引
                for (auto &[index_name, index_meta]: table_meta.indexes) {
                    char *key = new char[index_meta.col_tot_len];
                    for (auto &[index_offset, col_meta]: index_meta.cols) {
                        memcpy(key + index_offset, record.data + col_meta.offset, col_meta.len);
                    }
                    auto &&ih = sm_manager_->ihs_[index_name];
                    ih->delete_entry(key, txn);
                    delete []key;
                }
#ifdef ENABLE_LOGGING
                // 生成删除日志
                auto *delete_log_record = new DeleteLogRecord(txn->get_transaction_id(), record, rid, table_name);
                delete_log_record->prev_lsn_ = txn->get_prev_lsn();
                txn->set_prev_lsn(log_manager->add_log_to_buffer(delete_log_record));
                delete delete_log_record;
#endif
                break;
            }
            case WType::DELETE_TUPLE: {
                auto &record = write_record->GetRecord();
                auto &rid = write_record->GetRid();
                fh->insert_record(rid, record.data);
                // 插入索引
                for (auto &[index_name, index_meta]: table_meta.indexes) {
                    char *key = new char[index_meta.col_tot_len];
                    for (auto &[index_offset, col_meta]: index_meta.cols) {
                        memcpy(key + index_offset, record.data + col_meta.offset, col_meta.len);
                    }
                    auto &&ih = sm_manager_->ihs_[index_name];
                    ih->insert_entry(key, rid, txn);
                    delete []key;
                }
#ifdef ENABLE_LOGGING
                // 生成插入日志
                auto *insert_log_record = new InsertLogRecord(txn->get_transaction_id(), record, rid, table_name);
                insert_log_record->prev_lsn_ = txn->get_prev_lsn();
                txn->set_prev_lsn(log_manager->add_log_to_buffer(insert_log_record));
                delete insert_log_record;
#endif
                break;
            }
            case WType::UPDATE_TUPLE: {
                auto &old_record = write_record->GetRecord();
                auto &new_record = write_record->GetUpdatedRecord();
                auto &rid = write_record->GetRid();
                fh->update_record(rid, old_record.data, context);
                // 删除新索引，插入旧索引
                for (auto &[index_name, index_meta]: table_meta.indexes) {
                    char *old_key = new char[index_meta.col_tot_len];
                    char *new_key = new char[index_meta.col_tot_len];
                    for (auto &[index_offset, col_meta]: index_meta.cols) {
                        memcpy(old_key + index_offset, old_record.data + col_meta.offset, col_meta.len);
                        memcpy(new_key + index_offset, new_record.data + col_meta.offset, col_meta.len);
                    }
                    auto &&ih = sm_manager_->ihs_[index_name];
                    ih->delete_entry(new_key, txn);
                    ih->insert_entry(old_key, rid, txn);
                    delete []old_key;
                    delete []new_key;
                }
#ifdef ENABLE_LOGGING
                // 生成插入日志
                auto *update_log_record = new UpdateLogRecord(txn->get_transaction_id(), new_record, old_record, rid,
                                                              table_name);
                update_log_record->prev_lsn_ = txn->get_prev_lsn();
                txn->set_prev_lsn(log_manager->add_log_to_buffer(update_log_record));
                delete update_log_record;
#endif
                break;
            }
            default:
                throw InternalError("Unexpected WType！");
        }
        delete write_record;
    }
    delete context;
    write_set->clear();

    // 释放所有锁
    auto &&lock_set = txn->get_lock_set();
    for (auto &it: *lock_set) {
        lock_manager_->unlock(txn, it);
    }
    lock_set->clear();
#ifdef ENABLE_LOGGING
    auto *abort_log_record = new AbortLogRecord(txn->get_transaction_id());
    abort_log_record->prev_lsn_ = txn->get_prev_lsn();
    // TODO 日志管理
    txn->set_prev_lsn(log_manager->add_log_to_buffer(abort_log_record));
    log_manager->flush_log_to_disk();
    delete abort_log_record;
#endif
    txn->set_state(TransactionState::ABORTED);
}
