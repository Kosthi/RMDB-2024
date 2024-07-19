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

#include <atomic>
#include <deque>
#include <string>
#include <thread>
#include <unordered_set>
#include <memory>

#include "txn_defs.h"

/** Represents a link to a previous version of this tuple */
struct UndoLink {
    /* Previous version can be found in which txn */
    txn_id_t prev_txn_{INVALID_TXN_ID};
    /* The log index of the previous version in `prev_txn_` */
    int prev_log_idx_{0};

    friend auto operator==(const UndoLink &a, const UndoLink &b) {
        return a.prev_txn_ == b.prev_txn_ && a.prev_log_idx_ == b.prev_log_idx_;
    }

    friend auto operator!=(const UndoLink &a, const UndoLink &b) { return !(a == b); }

    /* Checks if the undo link points to something. */
    auto IsValid() const -> bool { return prev_txn_ != INVALID_TXN_ID; }
};

struct UndoLog {
    /* Whether this log is a deletion marker */
    bool is_deleted_;
    /* The fields modified by this undo log */
    std::vector<bool> modified_fields_;
    /* The modified fields */
    RmRecord tuple_;
    /* Timestamp of this undo log */
    timestamp_t ts_{INVALID_TIMESTAMP};
    /* Undo log prev version */
    UndoLink prev_version_{};
};

class Transaction {
public:
    friend class TransactionManager;

    explicit Transaction(txn_id_t txn_id, IsolationLevel isolation_level = IsolationLevel::SERIALIZABLE)
        : state_(TransactionState::DEFAULT), isolation_level_(isolation_level), txn_id_(txn_id) {
        write_set_ = std::make_shared<std::deque<WriteRecord *> >();
        lock_set_ = std::make_shared<std::unordered_set<LockDataId> >();
        index_latch_page_set_ = std::make_shared<std::deque<Page *> >();
        index_deleted_page_set_ = std::make_shared<std::deque<Page *> >();
        prev_lsn_ = INVALID_LSN;
        thread_id_ = std::this_thread::get_id();
    }

    ~Transaction() = default;

    inline txn_id_t get_transaction_id() { return txn_id_; }

    inline std::thread::id get_thread_id() { return thread_id_; }

    inline void set_txn_mode(bool txn_mode) { txn_mode_ = txn_mode; }
    inline bool get_txn_mode() { return txn_mode_; }

    inline void set_start_ts(timestamp_t start_ts) { start_ts_ = start_ts; }
    inline timestamp_t get_start_ts() { return start_ts_; }

    inline IsolationLevel get_isolation_level() { return isolation_level_; }

    inline TransactionState &get_state() { return state_; }
    inline void set_state(TransactionState state) { state_ = state; }

    inline lsn_t get_prev_lsn() { return prev_lsn_; }
    inline void set_prev_lsn(lsn_t prev_lsn) { prev_lsn_ = prev_lsn; }

    inline std::shared_ptr<std::deque<WriteRecord *> > get_write_set() { return write_set_; }
    inline void append_write_record(WriteRecord *write_record) { write_set_->push_back(write_record); }

    inline std::shared_ptr<std::deque<Page *> > get_index_deleted_page_set() { return index_deleted_page_set_; }
    inline void append_index_deleted_page(Page *page) { index_deleted_page_set_->push_back(page); }

    inline std::shared_ptr<std::deque<Page *> > get_index_latch_page_set() { return index_latch_page_set_; }
    inline void append_index_latch_page_set(Page *page) { index_latch_page_set_->push_back(page); }

    inline std::shared_ptr<std::unordered_set<LockDataId> > get_lock_set() { return lock_set_; }

    /** @return the read ts */
    inline auto GetReadTs() const -> timestamp_t { return read_ts_; }

    /** @return the commit ts */
    inline auto GetCommitTs() const -> timestamp_t { return commit_ts_; }

private:
    bool txn_mode_; // 用于标识当前事务为显式事务还是单条SQL语句的隐式事务
    TransactionState state_; // 事务状态
    IsolationLevel isolation_level_; // 事务的隔离级别，默认隔离级别为可串行化
    std::thread::id thread_id_; // 当前事务对应的线程id
    lsn_t prev_lsn_; // 当前事务执行的最后一条操作对应的lsn，用于系统故障恢复
    txn_id_t txn_id_; // 事务的ID，唯一标识符
    timestamp_t start_ts_; // 事务的开始时间戳
    /** The read ts */
    std::atomic<timestamp_t> read_ts_{0};
    /** The commit ts */
    std::atomic<timestamp_t> commit_ts_{INVALID_TIMESTAMP};

    std::shared_ptr<std::deque<WriteRecord *> > write_set_; // 事务包含的所有写操作
    std::shared_ptr<std::unordered_set<LockDataId> > lock_set_; // 事务申请的所有锁
    std::shared_ptr<std::deque<Page *> > index_latch_page_set_; // 维护事务执行过程中加锁的索引页面
    std::shared_ptr<std::deque<Page *> > index_deleted_page_set_; // 维护事务执行过程中删除的索引页面
};
