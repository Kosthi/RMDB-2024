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
#include <functional>
#include <unordered_map>

#include "transaction.h"
#include "recovery/log_manager.h"
#include "concurrency/lock_manager.h"
#include "concurrency/watermark.h"
#include "system/sm_manager.h"

/* 系统采用的并发控制算法，当前题目中要求两阶段封锁并发控制算法 */
enum class ConcurrencyMode { TWO_PHASE_LOCKING = 0, BASIC_TO };

/// The first undo link in the version chain, that links table heap tuple to the undo log.
struct VersionUndoLink {
    /** The next version in the version chain. */
    UndoLink prev_;
    /** Whether a transaction is modifying the version link. Fall 2023: you do not need to read / write this field until
     * task 4.2. */
    bool in_progress_{false};

    friend auto operator==(const VersionUndoLink &a, const VersionUndoLink &b) {
        return a.prev_ == b.prev_ && a.in_progress_ == b.in_progress_;
    }

    friend auto operator!=(const VersionUndoLink &a, const VersionUndoLink &b) { return !(a == b); }

    inline static auto FromOptionalUndoLink(std::optional<UndoLink> undo_link) -> std::optional<VersionUndoLink> {
        if (undo_link.has_value()) {
            return VersionUndoLink{*undo_link};
        }
        return std::nullopt;
    }
};

class TransactionManager {
public:
    explicit TransactionManager(LockManager *lock_manager, SmManager *sm_manager,
                                ConcurrencyMode concurrency_mode = ConcurrencyMode::TWO_PHASE_LOCKING) {
        sm_manager_ = sm_manager;
        lock_manager_ = lock_manager;
        concurrency_mode_ = concurrency_mode;
    }

    ~TransactionManager() = default;

    Transaction *begin(Transaction *txn, LogManager *log_manager);

    void commit(Transaction *txn, LogManager *log_manager);

    void abort(Transaction *txn, LogManager *log_manager);

    ConcurrencyMode get_concurrency_mode() { return concurrency_mode_; }

    void set_concurrency_mode(ConcurrencyMode concurrency_mode) { concurrency_mode_ = concurrency_mode; }

    LockManager *get_lock_manager() { return lock_manager_; }

    /**
     * @description: 获取事务ID为txn_id的事务对象
     * @return {Transaction*} 事务对象的指针
     * @param {txn_id_t} txn_id 事务ID
     */
    Transaction *get_transaction(txn_id_t txn_id) {
        if (txn_id == INVALID_TXN_ID) {
            return nullptr;
        }

        std::unique_lock lock(latch_);
        if (txn_map.find(txn_id) == txn_map.end()) {
            return nullptr;
        }

        auto *res = txn_map[txn_id];
        if (res->get_state() == TransactionState::COMMITTED || res->get_state() == TransactionState::ABORTED) {
            delete res;
            txn_map.erase(txn_id);
            return nullptr;
        }

        lock.unlock();
        assert(res != nullptr);
        assert(res->get_thread_id() == std::this_thread::get_id());

        return res;
    }

    static std::unordered_map<txn_id_t, Transaction *> txn_map; // 全局事务表，存放事务ID与事务对象的映射关系

    inline void set_next_txn_id(txn_id_t next_txn_id) { next_txn_id_.store(next_txn_id); }

    /**
  * @brief Use this function before task 4.2. Update an undo link that links table heap tuple to the first undo log.
  * Before updating, `check` function will be called to ensure validity.
  */
    auto UpdateUndoLink(Rid rid, std::optional<UndoLink> prev_link,
                        std::function<bool(std::optional<UndoLink>)> &&check = nullptr) -> bool;

    /**
     * @brief Use this function after task 4.2. Update an undo link that links table heap tuple to the first undo log.
     * Before updating, `check` function will be called to ensure validity.
     */
    auto UpdateVersionLink(Rid rid, std::optional<VersionUndoLink> prev_version,
                           std::function<bool(std::optional<VersionUndoLink>)> &&check = nullptr) -> bool;

    /** @brief Get the first undo log of a table heap tuple. Use this before task 4.2 */
    auto GetUndoLink(Rid rid) -> std::optional<UndoLink>;

    /** @brief Get the first undo log of a table heap tuple. Use this after task 4.2 */
    auto GetVersionLink(Rid rid) -> std::optional<VersionUndoLink>;

    /** @brief Access the transaction undo log buffer and get the undo log. Return nullopt if the txn does not exist. Will
     * still throw an exception if the index is out of range. */
    auto GetUndoLogOptional(UndoLink link) -> std::optional<UndoLog>;

    /** @brief Access the transaction undo log buffer and get the undo log. Except when accessing the current txn buffer,
     * you should always call this function to get the undo log instead of manually retrieve the txn shared_ptr and access
     * the buffer. */
    auto GetUndoLog(UndoLink link) -> UndoLog;

    /** @brief Get the lowest read timestamp in the system. */
    auto GetWatermark() -> timestamp_t { return running_txns_.GetWatermark(); }

    /** @brief Stop-the-world garbage collection. Will be called only when all transactions are not accessing the table
     * heap. */
    void GarbageCollection();

    struct PageVersionInfo {
        /** protects the map */
        std::shared_mutex mutex_;
        /** Stores previous version info for all slots. Note: DO NOT use `[x]` to access it because
         * it will create new elements even if it does not exist. Use `find` instead.
         */
        std::unordered_map<slot_offset_t, VersionUndoLink> prev_version_;
    };

    /** protects version info */
    std::shared_mutex version_info_mutex_;
    /** Stores the previous version of each tuple in the table heap. Do not directly access this field. Use the helper
 * functions in `transaction_manager_impl.cpp`. */
    std::unordered_map<page_id_t, std::shared_ptr<PageVersionInfo> > version_info_;

    /** Stores all the read_ts of running txns so as to facilitate garbage collection. */
    Watermark running_txns_{0};

    /** Only one txn is allowed to commit at a time */
    std::mutex commit_mutex_;
    /** The last committed timestamp. */
    std::atomic<timestamp_t> last_commit_ts_{0};

private:
    ConcurrencyMode concurrency_mode_; // 事务使用的并发控制算法，目前只需要考虑2PL
    std::atomic<txn_id_t> next_txn_id_{0}; // 用于分发事务ID
    std::atomic<timestamp_t> next_timestamp_{0}; // 用于分发事务时间戳
    std::mutex latch_; // 用于txn_map的并发
    SmManager *sm_manager_;
    LockManager *lock_manager_;
};
