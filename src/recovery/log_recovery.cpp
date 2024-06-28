/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"

#include <queue>
#include "transaction/transaction_manager.h"

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    // 逻辑递增，txn_id 和 lsn 都要恢复到 crash 前的状态
    lsn_t max_lsn = INVALID_LSN;
    txn_id_t max_txn_id = INVALID_TXN_ID;
    std::size_t log_offset = 0;
    // 这里返回的read_bytes <= LOG_BUFFER_SIZE
    int read_bytes;

    while ((read_bytes = disk_manager_->read_log(buffer_.buffer_, LOG_BUFFER_SIZE, log_offset)) > 0) {
        while (buffer_.offset_ + LOG_HEADER_SIZE <= read_bytes) {
            // 获取日志长度
            auto &log_size = *reinterpret_cast<const uint32_t *>(
                buffer_.buffer_ + buffer_.offset_ + OFFSET_LOG_TOT_LEN);
            // 日志完整内容需要从下次 read 中获取
            if (log_size > read_bytes - buffer_.offset_) {
                break;
            }

            auto &log_type = *reinterpret_cast<const LogType *>(buffer_.buffer_ + buffer_.offset_ + OFFSET_LOG_TYPE);
            switch (log_type) {
                case BEGIN: {
                    auto *log = new BeginLogRecord;
                    log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    active_txn_.emplace(log->log_tid_, log->lsn_);
                    // 在 log 文件中的 offset
                    lsn_mapping_.emplace(log->lsn_, log_offset + buffer_.offset_);
                    buffer_.offset_ += log->log_tot_len_;

                    // 找到 txn 和 lsn 最后的状态
                    max_lsn = std::max(max_lsn, log->lsn_);
                    max_txn_id = std::max(max_txn_id, log->log_tid_);

                    delete log;
                    break;
                }
                case COMMIT: {
                    auto *log = new CommitLogRecord;
                    log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    // 提交了则持久化到磁盘中了，不需要恢复
                    active_txn_.erase(log->log_tid_);
                    // 在 log 文件中的 offset
                    lsn_mapping_.emplace(log->lsn_, log_offset + buffer_.offset_);
                    buffer_.offset_ += log->log_tot_len_;
                    // 找到 txn 和 lsn 最后的状态
                    max_lsn = std::max(max_lsn, log->lsn_);
                    max_txn_id = std::max(max_txn_id, log->log_tid_);

                    delete log;
                    break;
                }
                case ABORT: {
                    auto *log = new AbortLogRecord;
                    log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    active_txn_.erase(log->log_tid_);
                    // 在 log 文件中的 offset
                    lsn_mapping_.emplace(log->lsn_, log_offset + buffer_.offset_);
                    buffer_.offset_ += log->log_tot_len_;
                    // 找到 txn 和 lsn 最后的状态
                    max_lsn = std::max(max_lsn, log->lsn_);
                    max_txn_id = std::max(max_txn_id, log->log_tid_);

                    delete log;
                    break;
                }
                case INSERT: {
                    auto *log = new InsertLogRecord;
                    log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    // emplace 如果存在会插入失败，用下标插入
                    active_txn_[log->log_tid_] = log->lsn_;
                    // 在 log 文件中的 offset
                    lsn_mapping_.emplace(log->lsn_, log_offset + buffer_.offset_);

                    auto fh = sm_manager_->fhs_.at(log->table_name_).get();
                    // 如果新建页面一次都没有落盘，可能会不存在
                    try {
                        fh->fetch_page_handle(log->rid_.page_no);
                    } catch (RMDBError &e) {
                        // throw InternalError(e.what());
                        fh->create_new_page_handle();
                    }
                    auto &&rm_page_handle = fh->fetch_page_handle(log->rid_.page_no);
                    // 判断需要 redo
                    if (rm_page_handle.page->get_page_lsn() < log->lsn_) {
                        dirty_page_table_.emplace_back(log->lsn_);
                        rm_page_handle.page->set_page_lsn(log->lsn_);
                    }
                    buffer_pool_manager_->unpin_page(rm_page_handle.page->get_page_id(), true);
                    buffer_pool_manager_->unpin_page(rm_page_handle.page->get_page_id(), true);

                    buffer_.offset_ += log->log_tot_len_;

                    // 找到 txn 和 lsn 最后的状态
                    max_lsn = std::max(max_lsn, log->lsn_);
                    max_txn_id = std::max(max_txn_id, log->log_tid_);

                    delete log;
                    break;
                }
                case DELETE: {
                    auto *log = new DeleteLogRecord;
                    log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    // emplace 如果存在会插入失败，用下标插入
                    active_txn_[log->log_tid_] = log->lsn_;
                    // 在 log 文件中的 offset
                    lsn_mapping_.emplace(log->lsn_, log_offset + buffer_.offset_);

                    auto fh = sm_manager_->fhs_.at(log->table_name_).get();
                    // 如果新建页面一次都没有落盘，可能会不存在
                    try {
                        fh->fetch_page_handle(log->rid_.page_no);
                    } catch (RMDBError &e) {
                        // throw InternalError(e.what());
                        fh->create_new_page_handle();
                    }
                    auto &&rm_page_handle = fh->fetch_page_handle(log->rid_.page_no);
                    // 判断需要 redo
                    if (rm_page_handle.page->get_page_lsn() < log->lsn_) {
                        dirty_page_table_.emplace_back(log->lsn_);
                        rm_page_handle.page->set_page_lsn(log->lsn_);
                    }
                    buffer_pool_manager_->unpin_page(rm_page_handle.page->get_page_id(), true);
                    buffer_pool_manager_->unpin_page(rm_page_handle.page->get_page_id(), true);

                    buffer_.offset_ += log->log_tot_len_;

                    // 找到 txn 和 lsn 最后的状态
                    max_lsn = std::max(max_lsn, log->lsn_);
                    max_txn_id = std::max(max_txn_id, log->log_tid_);

                    delete log;
                    break;
                }
                case UPDATE: {
                    auto *log = new UpdateLogRecord;
                    log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    // emplace 如果存在会插入失败，用下标插入
                    active_txn_[log->log_tid_] = log->lsn_;
                    // 在 log 文件中的 offset
                    lsn_mapping_.emplace(log->lsn_, log_offset + buffer_.offset_);

                    auto fh = sm_manager_->fhs_.at(log->table_name_).get();
                    // 如果新建页面一次都没有落盘，可能会不存在
                    try {
                        fh->fetch_page_handle(log->rid_.page_no);
                    } catch (RMDBError &e) {
                        // throw InternalError(e.what());
                        fh->create_new_page_handle();
                    }
                    auto &&rm_page_handle = fh->fetch_page_handle(log->rid_.page_no);
                    // 判断需要 redo
                    if (rm_page_handle.page->get_page_lsn() < log->lsn_) {
                        dirty_page_table_.emplace_back(log->lsn_);
                        rm_page_handle.page->set_page_lsn(log->lsn_);
                    }
                    buffer_pool_manager_->unpin_page(rm_page_handle.page->get_page_id(), true);
                    buffer_pool_manager_->unpin_page(rm_page_handle.page->get_page_id(), true);

                    buffer_.offset_ += log->log_tot_len_;

                    // 找到 txn 和 lsn 最后的状态
                    max_lsn = std::max(max_lsn, log->lsn_);
                    max_txn_id = std::max(max_txn_id, log->log_tid_);

                    delete log;
                    break;
                }
                default:
                    break;
            }
        }
        // read_bytes - (read_bytes - offset)
        log_offset += buffer_.offset_;
        buffer_.offset_ = 0;
        memset(buffer_.buffer_, 0, sizeof(LOG_BUFFER_SIZE));
    }
    log_manager_->set_global_lsn(max_lsn + 1);
    log_manager_->set_persist_lsn(max_lsn);
    transaction_manager_->set_next_txn_id(max_txn_id + 1);
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    for (auto &lsn: dirty_page_table_) {
        int log_offset = lsn_mapping_[lsn];
        disk_manager_->read_log(buffer_.buffer_, LOG_BUFFER_SIZE, log_offset);
        auto &log_type = *reinterpret_cast<const LogType *>(buffer_.buffer_ + OFFSET_LOG_TYPE);
        switch (log_type) {
            case INSERT: {
                auto log = new InsertLogRecord;
                log->deserialize(buffer_.buffer_);

                // redo 记录
                auto fh = sm_manager_->fhs_.at(log->table_name_).get();
                fh->insert_record(log->rid_, log->insert_value_.data);

                // redo 索引
                // auto &indexes = sm_manager_->db_.get_table(log->table_name_).indexes;
                // for (auto &[index_name, index_meta]: indexes) {
                //     int offset = 0;
                //     char *key = new char[index_meta.col_tot_len];
                //     auto &ih = sm_manager_->ihs_.at(index_name);
                //     for (auto &col_meta: index_meta.cols) {
                //         memcpy(key + offset, log->insert_value_.data + col_meta.offset, col_meta.len);
                //         offset += col_meta.len;
                //     }
                //     ih->insert_entry(key, log->rid_, &transaction_);
                //     delete []key;
                // }

                delete log;
                break;
            }
            case DELETE: {
                auto log = new DeleteLogRecord;
                log->deserialize(buffer_.buffer_);

                auto fh = sm_manager_->fhs_.at(log->table_name_).get();
                try {
                    fh->delete_record(log->rid_, nullptr);
                } catch (RecordNotFoundError& e) {

                }

                // redo 索引
                // auto &indexes = sm_manager_->db_.get_table(log->table_name_).indexes;
                // for (auto &[index_name, index_meta]: indexes) {
                //     int offset = 0;
                //     char *key = new char[index_meta.col_tot_len];
                //     auto &ih = sm_manager_->ihs_.at(index_name);
                //     for (auto &col_meta: index_meta.cols) {
                //         memcpy(key + offset, log->delete_value_.data + col_meta.offset, col_meta.len);
                //         offset += col_meta.len;
                //     }
                //     ih->delete_entry(key, &transaction_);
                //     delete []key;
                // }

                delete log;
                break;
            }
            case UPDATE: {
                auto log = new UpdateLogRecord;
                log->deserialize(buffer_.buffer_ + buffer_.offset_);

                auto fh = sm_manager_->fhs_.at(log->table_name_).get();
                fh->update_record(log->rid_, log->update_value_.data, nullptr);

                // redo 索引
                // auto &indexes = sm_manager_->db_.get_table(log->table_name_).indexes;
                // for (auto &[index_name, index_meta]: indexes) {
                //     int offset = 0;
                //     char *old_key = new char[index_meta.col_tot_len];
                //     char *new_key = new char[index_meta.col_tot_len];
                //     auto &ih = sm_manager_->ihs_.at(index_name);
                //     for (auto &col_meta: index_meta.cols) {
                //         memcpy(old_key + offset, log->old_value_.data + col_meta.offset, col_meta.len);
                //         memcpy(new_key + offset, log->update_value_.data + col_meta.offset, col_meta.len);
                //         offset += col_meta.len;
                //     }
                //     ih->delete_entry(old_key, &transaction_);
                //     ih->insert_entry(new_key, log->rid_, &transaction_);
                //     delete []old_key;
                //     delete []new_key;
                // }

                delete log;
                break;
            }
            default:
                break;
        }
    }
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    // redo 从前往后 undo 从后往前
    // 用大根堆来维护
    std::priority_queue<lsn_t> lsn_heap;

    for (auto &[_, lsn]: active_txn_) {
        lsn_heap.emplace(lsn);
    }

    lsn_t lsn;
    while (!lsn_heap.empty()) {
        lsn = lsn_heap.top();
        lsn_heap.pop();
        int log_offset = lsn_mapping_[lsn];
        disk_manager_->read_log(buffer_.buffer_, LOG_BUFFER_SIZE, log_offset);
        auto &log_type = *reinterpret_cast<const LogType *>(buffer_.buffer_ + OFFSET_LOG_TYPE);
        switch (log_type) {
            case BEGIN: {
                auto log = new BeginLogRecord;
                log->deserialize(buffer_.buffer_);
                lsn = log->prev_lsn_;
                delete log;
                break;
            }
            case COMMIT: {
                auto log = new CommitLogRecord;
                log->deserialize(buffer_.buffer_);
                lsn = log->prev_lsn_;
                delete log;
                break;
            }
            case ABORT: {
                auto log = new AbortLogRecord;
                log->deserialize(buffer_.buffer_);
                lsn = log->prev_lsn_;
                delete log;
                break;
            }
            case INSERT: {
                auto log = new InsertLogRecord;
                log->deserialize(buffer_.buffer_);

                // undo 记录
                auto fh = sm_manager_->fhs_.at(log->table_name_).get();
                fh->delete_record(log->rid_, nullptr);

                // undo 索引
                // auto &indexes = sm_manager_->db_.get_table(log->table_name_).indexes;
                // for (auto &[index_name, index_meta]: indexes) {
                //     int offset = 0;
                //     char *key = new char[index_meta.col_tot_len];
                //     auto &ih = sm_manager_->ihs_.at(index_name);
                //     for (auto &col_meta: index_meta.cols) {
                //         memcpy(key + offset, log->insert_value_.data + col_meta.offset, col_meta.len);
                //         offset += col_meta.len;
                //     }
                //     ih->delete_entry(key, &transaction_);
                //     delete []key;
                // }

                lsn = log->prev_lsn_;
                delete log;
                break;
            }
            case DELETE: {
                auto log = new DeleteLogRecord;
                log->deserialize(buffer_.buffer_);

                auto fh = sm_manager_->fhs_.at(log->table_name_).get();
                fh->insert_record(log->rid_, log->delete_value_.data);

                // undo 索引
                // auto &indexes = sm_manager_->db_.get_table(log->table_name_).indexes;
                // for (auto &[index_name, index_meta]: indexes) {
                //     int offset = 0;
                //     char *key = new char[index_meta.col_tot_len];
                //     auto &ih = sm_manager_->ihs_.at(index_name);
                //     for (auto &col_meta: index_meta.cols) {
                //         memcpy(key + offset, log->delete_value_.data + col_meta.offset, col_meta.len);
                //         offset += col_meta.len;
                //     }
                //     ih->insert_entry(key, log->rid_, &transaction_);
                //     delete []key;
                // }

                lsn = log->prev_lsn_;
                delete log;
                break;
            }
            case UPDATE: {
                auto log = new UpdateLogRecord;
                log->deserialize(buffer_.buffer_);

                // undo 记录
                auto fh = sm_manager_->fhs_.at(log->table_name_).get();
                fh->update_record(log->rid_, log->old_value_.data, nullptr);

                // undo 索引
                // auto &indexes = sm_manager_->db_.get_table(log->table_name_).indexes;
                // for (auto &[index_name, index_meta]: indexes) {
                //     int offset = 0;
                //     char *old_key = new char[index_meta.col_tot_len];
                //     char *new_key = new char[index_meta.col_tot_len];
                //     auto &ih = sm_manager_->ihs_.at(index_name);
                //     for (auto &col_meta: index_meta.cols) {
                //         memcpy(old_key + offset, log->old_value_.data + col_meta.offset, col_meta.len);
                //         memcpy(new_key + offset, log->update_value_.data + col_meta.offset, col_meta.len);
                //         offset += col_meta.len;
                //     }
                //     ih->delete_entry(new_key, &transaction_);
                //     ih->insert_entry(old_key, log->rid_, &transaction_);
                //     delete []old_key;
                //     delete []new_key;
                // }

                lsn = log->prev_lsn_;
                delete log;
                break;
            }
            default:
                break;
        }

        if (lsn != INVALID_LSN) {
            lsn_heap.emplace(lsn);
        }
    }

    // 重做索引
    if (!dirty_page_table_.empty() || !active_txn_.empty()) {
        redo_indexes();
    }
}

/**
 * @description: 重做每个表的索引
 */
void RecoveryManager::redo_indexes() {
    std::vector<std::string> col_names;
    auto *context = new Context(nullptr, nullptr, &transaction_);
    for (auto &[table_name, _]: sm_manager_->fhs_) {
        auto &table_meta = sm_manager_->db_.get_table(table_name);
        for (auto &[index_name, index_meta]: table_meta.indexes) {
            for (auto &[_, col]: index_meta.cols) {
                col_names.emplace_back(col.name);
            }
            sm_manager_->redo_index(table_name, table_meta, col_names, index_name, context);
            col_names.clear();
        }
    }
    delete context;
}
