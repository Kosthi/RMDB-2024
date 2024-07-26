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

#include <mutex>
#include <condition_variable>
#include <stack>

#include "transaction/transaction.h"

static const std::string GroupLockModeStr[10] = {"NON_LOCK", "IS", "IX", "S", "SIX", "X"};

class TransactionManager;

class LockManager {
/* 加锁类型，包括共享锁、排他锁、意向共享锁、意向排他锁、SIX（意向排他锁+共享锁） */
public:
    TransactionManager *txn_manager;

    enum class LockMode { INTENTION_SHARED, INTENTION_EXCLUSIVE, SHARED, S_IX, EXCLUSIVE };

    /* 用于标识加锁队列中排他性最强的锁类型，例如加锁队列中有SHARED和EXLUSIVE两个加锁操作，则该队列的锁模式为X */
    enum class GroupLockMode { NON_LOCK, IS, IX, S, SIX, X };

    /* 事务的加锁申请 */
    class LockRequest {
    public:
        LockRequest(txn_id_t txn_id, LockMode lock_mode, bool granted = false)
            : txn_id_(txn_id), lock_mode_(lock_mode), granted_(granted) {
        }

        txn_id_t txn_id_; // 申请加锁的事务ID
        LockMode lock_mode_; // 事务申请加锁的类型
        bool granted_; // 该事务是否已经被赋予锁
    };

    /* 数据项上的加锁队列 */
    class LockRequestQueue {
    public:
        std::list<LockRequest> request_queue_; // 加锁队列
        std::condition_variable cv_; // 条件变量，用于唤醒正在等待加锁的申请，在no-wait策略下无需使用
        GroupLockMode group_lock_mode_ = GroupLockMode::NON_LOCK; // 加锁队列的锁模式
        bool upgrading_ = false;
        int shared_lock_num_ = 0;
        int IX_lock_num_ = 0;
        txn_id_t oldest_txn_id_ = INT32_MAX; // 维护等待队列中最老（时间戳最小）的事务id
    };

    void StartDeadlockDetection() {
        enable_cycle_detection_ = true;
        cycle_detection_thread_ = new std::thread(&LockManager::RunCycleDetection, this);
    }

    auto DFS(std::vector<txn_id_t> cycle_vector, bool &is_cycle, txn_id_t *txn_id) -> void;

    /*** Graph API ***/

    /**
     * Adds an edge from t1 -> t2 from waits for graph.
     * @param t1 transaction waiting for a lock
     * @param t2 transaction being waited for
     */
    auto AddEdge(txn_id_t t1, txn_id_t t2) -> void;

    /**
     * Removes an edge from t1 -> t2 from waits for graph.
     * @param t1 transaction waiting for a lock
     * @param t2 transaction being waited for
     */
    auto RemoveEdge(txn_id_t t1, txn_id_t t2) -> void;

    /**
     * Checks if the graph has a cycle, returning the newest transaction ID in the cycle if so.
     * @param[out] txn_id if the graph has a cycle, will contain the newest transaction ID
     * @return false if the graph has no cycle, otherwise stores the newest transaction ID in the cycle to txn_id
     */
    auto HasCycle(txn_id_t *txn_id) -> bool;

    /**
     * @return all edges in current waits_for graph
     */
    auto GetEdgeList() -> std::vector<std::pair<txn_id_t, txn_id_t>>;

    /**
     * Runs cycle detection in the background.
     */
    auto RunCycleDetection() -> void;

    auto Dfs(txn_id_t txn_id, std::unordered_map<txn_id_t, int> &mp, std::stack<txn_id_t> &stk,
                      std::unordered_map<txn_id_t, int> &ump) -> bool;

    std::atomic<bool> enable_cycle_detection_;
    std::thread *cycle_detection_thread_;
    /** Waits-for graph representation. */
    std::unordered_map<txn_id_t, std::vector<txn_id_t>> waits_for_;
    std::mutex waits_for_latch_;

public:
    explicit LockManager(): enable_cycle_detection_(true), cycle_detection_thread_(nullptr) {
        StartDeadlockDetection();
    }

    ~LockManager() = default;

    bool lock_shared_on_gap(Transaction *txn, IndexMeta &index_meta, Gap &gap, int tab_fd);

    bool lock_exclusive_on_gap(Transaction *txn, IndexMeta &index_meta, Gap &gap, int tab_fd);

    bool isSafeInGap(Transaction *txn, IndexMeta &index_meta, RmRecord &record);

    bool lock_shared_on_record(Transaction *txn, const Rid &rid, int tab_fd);

    bool lock_exclusive_on_record(Transaction *txn, const Rid &rid, int tab_fd);

    bool lock_shared_on_table(Transaction *txn, int tab_fd);

    bool lock_exclusive_on_table(Transaction *txn, int tab_fd);

    bool lock_IS_on_table(Transaction *txn, int tab_fd);

    bool lock_IX_on_table(Transaction *txn, int tab_fd);

    bool unlock(Transaction *txn, const LockDataId &lock_data_id);

private:
    std::mutex latch_; // 用于锁表的并发
    std::unordered_map<LockDataId, LockRequestQueue> lock_table_; // 全局锁表
    std::unordered_map<IndexMeta, std::unordered_map<LockDataId, LockRequestQueue> > gap_lock_table_; // 全局间隙锁表
};
