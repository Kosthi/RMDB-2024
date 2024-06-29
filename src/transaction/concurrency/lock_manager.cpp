/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

// 加锁阶段检查
static inline bool check_lock(Transaction *txn) {
    auto &txn_state = txn->get_state();
    // 事务结束，不能再获取锁
    if (txn_state == TransactionState::COMMITTED || txn_state == TransactionState::ABORTED) {
        return false;
    }
    // 两阶段锁，收缩阶段不允许加锁
    if (txn_state == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    // 没加过锁，更新事务为增长状态，开始两阶段锁第一阶段
    if (txn_state == TransactionState::DEFAULT) {
        txn_state = TransactionState::GROWING;
    }
    return true;
}

/**
 * @description: 申请间隙锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rmcord&} rid 加锁的间隙下限记录
 * @param {Rmcord&} rid 加锁的间隙上限记录
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_gap(Transaction *txn, IndexMeta &index_meta, Gap &gap, int tab_fd) {
    std::lock_guard lock(latch_);

    if (!check_lock(txn)) {
        return false;
    }

    LockDataId lock_data_id(tab_fd, index_meta, gap, LockDataType::GAP);
    auto &&it = gap_lock_table_.find(index_meta);
    if (it == gap_lock_table_.end()) {
        // 新建
        it = gap_lock_table_.emplace(std::piecewise_construct, std::forward_as_tuple(index_meta),
                                     std::forward_as_tuple()).first;
        it->second.emplace(std::piecewise_construct, std::forward_as_tuple(lock_data_id),
                           std::forward_as_tuple());
    }

    if (it->second.find(lock_data_id) == it->second.end()) {
        it->second.emplace(std::piecewise_construct, std::forward_as_tuple(lock_data_id),
                           std::forward_as_tuple());
    }

    auto &lock_request_queue = it->second.at(lock_data_id);
    for (auto &lock_request: lock_request_queue.request_queue_) {
        // 如果锁请求队列上该事务已经有共享锁或更高级别的锁（X）了，加锁成功
        // 得到锁，S 或 X 且不存在间隙冲突 通过
        // 没得到锁，阻塞，不可能执行到这里
        if (lock_request.txn_id_ == txn->get_transaction_id()) {
            // 事务能执行到这里，要么第一次申请，要么等待结束了，拿到锁了
            assert(lock_request.granted_);
            return true;
        }
    }

    // 第一次申请，检查锁队列中有没有冲突的事务
    // insert 算子检查是否在间隙里
    // delete 算子在index_scan中检查是否在间隙里
    // 检查间隙是否相交
    bool contain_X = false;
    for (auto &[data_id, queue]: it->second) {
        if (queue.group_lock_mode_ == GroupLockMode::X && gap.isCoincide(data_id.gap_)) {
            contain_X = true;
            break;
        }
    }

    // 发生冲突
    if (lock_request_queue.group_lock_mode_ == GroupLockMode::X || contain_X) {
        // delete 算子
        // 阻塞等待

        if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }

        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
        lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::SHARED);
        std::unique_lock ul(latch_, std::adopt_lock);

        auto &&cur = lock_request_queue.request_queue_.begin();

        // 通过条件：当前请求队列只有共享间隙锁且相交区间不存在 X 锁
        lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur, &it, &lock_data_id]() {
            for (auto &&it_ = lock_request_queue.request_queue_.begin(); it_ != lock_request_queue.request_queue_.end();
                 ++it_) {
                if (it_->txn_id_ != txn->get_transaction_id()) {
                    if (it_->lock_mode_ != LockMode::SHARED || it_->granted_) {
                        return false;
                    }
                } else {
                    cur = it_;
                    break;
                }
            }

            bool contain_X = false;
            for (auto &[data_id, queue]: it->second) {
                if (queue.group_lock_mode_ == GroupLockMode::X && lock_data_id.gap_.isCoincide(data_id.gap_)) {
                    contain_X = true;
                    break;
                }
            }
            return !contain_X;
        });

        cur->granted_ = true;
        lock_request_queue.group_lock_mode_ = GroupLockMode::S;
        ++lock_request_queue.shared_lock_num_;
        txn->get_lock_set()->emplace(lock_data_id);
        ul.release();
        return true;
    }

    // 每次事务申请锁都要更新最老事务id
    if (txn->get_transaction_id() < lock_request_queue.oldest_txn_id_) {
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
    }

    // 将当前事务锁请求加到锁请求队列中
    lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::SHARED, true);
    // 更新锁请求队列锁模式为共享锁
    lock_request_queue.group_lock_mode_ = GroupLockMode::S;
    ++lock_request_queue.shared_lock_num_;
    txn->get_lock_set()->emplace(lock_data_id);
    return true;
}

// insert 和 delete 算子会调用
/**
 * @description: 申请间隙锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rmcord&} rid 加锁的间隙下限记录
 * @param {Rmcord&} rid 加锁的间隙上限记录
 * @param {int} tab_fd
 */
bool LockManager::lock_exclusive_on_gap(Transaction *txn, IndexMeta &index_meta, Gap &gap, int tab_fd) {
    std::lock_guard lock(latch_);

    if (!check_lock(txn)) {
        return false;
    }

    LockDataId lock_data_id(tab_fd, index_meta, gap, LockDataType::GAP);
    auto &&it = gap_lock_table_.find(index_meta);
    if (it == gap_lock_table_.end()) {
        // 新建
        it = gap_lock_table_.emplace(std::piecewise_construct, std::forward_as_tuple(index_meta),
                                     std::forward_as_tuple()).first;
    }

    if (it->second.find(lock_data_id) == it->second.end()) {
        it->second.emplace(std::piecewise_construct, std::forward_as_tuple(lock_data_id),
                           std::forward_as_tuple());
    }

    bool contain = false;
    // 独占锁只要有区间相交就得等待
    for (auto &[data_id, queue]: it->second) {
        if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK) {
            if (gap.isCoincide(data_id.gap_)) {
                bool is_only_txn = true;
                for (auto &req: queue.request_queue_) {
                    if (req.txn_id_ != txn->get_transaction_id() && req.granted_) {
                        is_only_txn = false;
                        break;
                    }
                }
                if (!is_only_txn) {
                    contain = true;
                    break;
                }
            }
        }
    }

    auto &lock_request_queue = it->second.at(lock_data_id);
    for (auto &lock_request: lock_request_queue.request_queue_) {
        // 如果锁请求队列上该事务已经有共享锁或更高级别的锁（X）了，加锁成功
        if (lock_request.txn_id_ == txn->get_transaction_id()) {
            assert(lock_request.granted_);
            // 有间隙 X 锁
            if (lock_request.lock_mode_ == LockMode::EXCLUSIVE) {
                return true;
            }

            assert(lock_request.lock_mode_ == LockMode::SHARED);

            // 有间隙 S 锁，且队列中只有自己拿到 X 锁
            if (lock_request_queue.shared_lock_num_ == 1 && !contain) {
                lock_request.lock_mode_ = LockMode::EXCLUSIVE;
                lock_request_queue.shared_lock_num_ = 0;
                lock_request_queue.group_lock_mode_ = GroupLockMode::X;
                return true;
            }

            if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }

            lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
            lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXCLUSIVE);

            std::unique_lock ul(latch_, std::adopt_lock);
            auto &&cur = lock_request_queue.request_queue_.begin();
            // 通过条件：当前请求之前没有任何已授权的请求并且不存在相交区间
            lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur, &it, &lock_data_id]() {
                // for (auto &&it_ = lock_request_queue.request_queue_.begin(); it_ != lock_request_queue.request_queue_.end();
                //      ++it_) {
                //     if (it_->txn_id_ != txn->get_transaction_id()) {
                //         if (it_->granted_) {
                //             return false;
                //         }
                //     } else {
                //         cur = it_;
                //         break;
                //     }
                // }
                if (lock_request_queue.shared_lock_num_ != 1) {
                    return false;
                }

                assert(lock_request_queue.shared_lock_num_ == 1);

                bool contain = false;
                for (auto &[data_id, queue]: it->second) {
                    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK) {
                        if (lock_data_id.gap_.isCoincide(data_id.gap_)) {
                            bool is_only_txn = true;
                            for (auto &req: queue.request_queue_) {
                                if (req.txn_id_ != txn->get_transaction_id() && req.granted_) {
                                    is_only_txn = false;
                                    break;
                                }
                            }
                            if (!is_only_txn) {
                                contain = true;
                                break;
                            }
                        }
                    }
                }
                return !contain;
            });
            cur->granted_ = true;
            lock_request_queue.group_lock_mode_ = GroupLockMode::X;
            txn->get_lock_set()->emplace(lock_data_id);
            ul.release();
            return true;
        }
    }

    // 第一次申请，检查锁队列中有没有冲突的事务
    // insert 算子检查是否在间隙里
    // delete 算子在index_scan中检查是否在间隙里
    // 检查间隙是否相交
    // bool contain = false;
    // // 独占锁只要有区间相交就得等待
    // for (auto &[data_id, queue]: it->second) {
    //     if (gap.isCoincide(data_id.gap_)) {
    //         contain = true;
    //         break;
    //     }
    // }

    // 发生冲突
    if (lock_request_queue.group_lock_mode_ != GroupLockMode::NON_LOCK || contain) {
        // insert/delete 算子
        // 阻塞等待

        // TODO 间隙相交的队列要检查吗？
        if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }

        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
        lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXCLUSIVE);

        std::unique_lock ul(latch_, std::adopt_lock);
        auto &&cur = lock_request_queue.request_queue_.begin();
        // 通过条件：当前请求之前没有任何已授权的请求并且不存在相交区间
        // 后面没有通过的 S 锁
        lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur, &it, &lock_data_id]() {
            for (auto &&it_ = lock_request_queue.request_queue_.begin(); it_ != lock_request_queue.request_queue_.end();
                 ++it_) {
                if (it_->txn_id_ != txn->get_transaction_id()) {
                    if (it_->granted_) {
                        return false;
                    }
                } else {
                    cur = it_;
                    break;
                }
            }

            if (lock_request_queue.shared_lock_num_ != 0) {
                return false;
            }

            assert(lock_request_queue.shared_lock_num_ == 0);

            bool contain = false;
            for (auto &[data_id, queue]: it->second) {
                if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK) {
                    if (lock_data_id.gap_.isCoincide(data_id.gap_)) {
                        bool is_only_txn = true;
                        for (auto &req: queue.request_queue_) {
                            if (req.txn_id_ != txn->get_transaction_id() && req.granted_) {
                                is_only_txn = false;
                                break;
                            }
                        }
                        if (!is_only_txn) {
                            contain = true;
                            break;
                        }
                    }
                }
            }
            return !contain;
        });
        cur->granted_ = true;
        lock_request_queue.group_lock_mode_ = GroupLockMode::X;
        txn->get_lock_set()->emplace(lock_data_id);
        ul.release();
        return true;
    }

    // 每次事务申请锁都要更新最老事务id
    if (txn->get_transaction_id() < lock_request_queue.oldest_txn_id_) {
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
    }

    // 将当前事务锁请求加到锁请求队列中
    lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXCLUSIVE, true);
    // 更新锁请求队列锁模式为 X 锁
    lock_request_queue.group_lock_mode_ = GroupLockMode::X;
    txn->get_lock_set()->emplace(lock_data_id);
    return true;
}

// insert 算子会调用，但是上行锁
/**
 * @description: 申请间隙锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rmcord&} rid 加锁的间隙下限记录
 * @param {Rmcord&} rid 加锁的间隙上限记录
 * @param {int} tab_fd
 */
bool LockManager::isSafeInGap(Transaction *txn, IndexMeta &index_meta, RmRecord &record) {
    std::lock_guard lock(latch_);

    // if (!check_lock(txn)) {
    //     return false;
    // }

    auto &&it = gap_lock_table_.find(index_meta);
    if (it == gap_lock_table_.end()) {
        return true;
    }

    // if (it->second.find(lock_data_id) == it->second.end()) {
    //     it->second.emplace(std::piecewise_construct, std::forward_as_tuple(lock_data_id),
    //                        std::forward_as_tuple());
    // }
    //
    // auto &lock_request_queue = it->second.at(lock_data_id);
    // for (auto &lock_request: lock_request_queue.request_queue_) {
    //     // 如果锁请求队列上该事务已经有共享锁或更高级别的锁（X）了，加锁成功
    //     if (lock_request.txn_id_ == txn->get_transaction_id()) {
    //         // 事务能执行到这里，要么第一次申请，要么等待结束了，拿到锁了
    //         assert(lock_request.granted_);
    //         return true;
    //     }
    // }

    // insert 算子检查记录是否在间隙里
    // bool contain = false;
    // bool contain_X = false;

    // 独占锁只要有区间相交就得等待
    for (auto &[data_id, queue]: it->second) {
        if (data_id.gap_.isInGap(record)) {
            bool is_only_txn = true;
            for (auto &req: queue.request_queue_) {
                if (req.txn_id_ != txn->get_transaction_id() && req.granted_) {
                    is_only_txn = false;
                    break;
                }
            }
            // 队列中没有其他事务取得锁，则当前事务一定拿到了锁（如果没拿到锁阻塞也不可能执行到这里），那么就可以插入
            if (is_only_txn) {
                continue;
            }

            // 当前事务独占 TODO may bugs
            // if (queue.request_queue_.size() == 1 && queue.request_queue_.begin()->txn_id_ == txn->
            //     get_transaction_id()) {
            //     continue;
            // }

            // wait-die
            if (txn->get_transaction_id() > queue.oldest_txn_id_) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }

            // assert(queue.oldest_txn_id_ == txn->get_transaction_id());

            // queue.oldest_txn_id_ = txn->get_transaction_id();
            // queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXCLUSIVE);

            std::unique_lock ul(latch_, std::adopt_lock);
            auto &&cur = queue.request_queue_.begin();
            // 通过条件：当前请求之前没有任何已授权的请求并且不存在相交区间
            queue.cv_.wait(ul, [&queue, txn, &cur, &it, &record]() {
                for (auto &req: queue.request_queue_) {
                    if (req.txn_id_ != txn->get_transaction_id() && req.granted_) {
                        return false;
                    }
                }
                return true;

                // for (auto &&it_ = queue.request_queue_.begin(); it_ != queue.request_queue_.end();
                //      ++it_) {
                //     if (it_->txn_id_ != txn->get_transaction_id()) {
                //         if (it_->granted_) {
                //             return false;
                //         }
                //     } else {
                //         cur = it_;
                //         break;
                //     }
                // }
                // return true;
            });
            ul.release();
        }
    }
    return true;
}

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    std::lock_guard lock(latch_);

    if (!check_lock(txn)) {
        return false;
    }

    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    auto &&it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) {
        it = lock_table_.emplace(std::piecewise_construct, std::forward_as_tuple(lock_data_id),
                                 std::forward_as_tuple()).first;
        it->second.oldest_txn_id_ = txn->get_transaction_id();
    }

    auto &lock_request_queue = it->second;
    for (auto &lock_request: lock_request_queue.request_queue_) {
        // 如果锁请求队列上该事务已经有共享锁或更高级别的锁（X）了，加锁成功
        if (lock_request.txn_id_ == txn->get_transaction_id()) {
            // 事务能执行到这里，要么第一次申请，要么等待结束了，拿到锁了
            assert(lock_request.granted_);
            return true;
        }
    }

    // 如果其他事务有 X 锁，加锁失败（no-wait）
    // if (lock_request_queue.group_lock_mode_ == GroupLockMode::X || lock_request_queue.group_lock_mode_ == GroupLockMode::IX || lock_request_queue.group_lock_mode_ == GroupLockMode::SIX) {
    //     lock_request_queue.cv_.notify_all();
    //     throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    // }

    // 第一次申请，检查锁队列中有没有冲突的事务
    // Check for conflicting locks and apply wait-die logic
    if (lock_request_queue.group_lock_mode_ == GroupLockMode::X || lock_request_queue.group_lock_mode_ ==
        GroupLockMode::IX || lock_request_queue.group_lock_mode_ == GroupLockMode::SIX) {
        if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
        lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::SHARED);
        std::unique_lock<std::mutex> ul(latch_, std::adopt_lock);
        auto &&cur = lock_request_queue.request_queue_.begin();
        lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur]() {
            for (auto &&it = lock_request_queue.request_queue_.begin(); it != lock_request_queue.request_queue_.end();
                 ++it) {
                if (it->txn_id_ != txn->get_transaction_id()) {
                    if (it->lock_mode_ != LockMode::SHARED || it->granted_) {
                        return false;
                    }
                } else {
                    cur = it;
                    break;
                }
            }
            return true;
        });
        cur->granted_ = true;
        lock_request_queue.group_lock_mode_ = static_cast<GroupLockMode>(std::max(
            static_cast<int>(GroupLockMode::S), static_cast<int>(lock_request_queue.group_lock_mode_)));
        ++lock_request_queue.shared_lock_num_;
        txn->get_lock_set()->emplace(lock_data_id);
        ul.release();
        return true;
    }

    // 每次事务申请锁都要更新最老事务id
    if (txn->get_transaction_id() < lock_request_queue.oldest_txn_id_) {
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
    }

    // 将当前事务锁请求加到锁请求队列中
    lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::SHARED, true);
    // 更新锁请求队列锁模式为共享锁
    lock_request_queue.group_lock_mode_ = GroupLockMode::S;
    ++lock_request_queue.shared_lock_num_;
    txn->get_lock_set()->emplace(lock_data_id);
    return true;
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    std::lock_guard lock(latch_);

    if (!check_lock(txn)) {
        return false;
    }

    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    auto &&it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) {
        it = lock_table_.emplace(std::piecewise_construct, std::forward_as_tuple(lock_data_id),
                                 std::forward_as_tuple()).first;
        it->second.oldest_txn_id_ = txn->get_transaction_id();
    }

    auto &lock_request_queue = it->second;
    for (auto &lock_request: lock_request_queue.request_queue_) {
        // 该事务上的锁请求队列上已经有互斥锁了，加锁成功
        if (lock_request.txn_id_ == txn->get_transaction_id()) {
            assert(lock_request.granted_);
            if (lock_request.lock_mode_ == LockMode::EXCLUSIVE) {
                return true;
            }
            // 如果当前记录没有其他事务在读，升级写锁
            if (lock_request.lock_mode_ == LockMode::SHARED && lock_request_queue.request_queue_.size() == 1) {
                lock_request.lock_mode_ = LockMode::EXCLUSIVE;
                lock_request_queue.group_lock_mode_ = GroupLockMode::X;
                lock_request_queue.shared_lock_num_ = 0;
                return true;
            }

            assert(lock_request.lock_mode_ == LockMode::SHARED);
            // 整个队列的时间戳不一定严格降序，需比较其中最老的事务id，用一个 oldest_txn_id_ 变量来维护，且等待队列中的处于等待的当前事务不可能还会申请其他锁了（阻塞）
            // 无论有没有得到锁都要先进入等待队列，得到锁后 granted_ 置真
            if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
                // Younger transaction requests the lock, abort the current transaction
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }

            lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
            lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXCLUSIVE);
            std::unique_lock<std::mutex> ul(latch_, std::adopt_lock);
            auto &&cur = lock_request_queue.request_queue_.begin();
            // 通过条件：当前请求之前没有任何已授权的请求
            lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur]() {
                for (auto &&it = lock_request_queue.request_queue_.begin();
                     it != lock_request_queue.request_queue_.end(); ++it) {
                    if (it->txn_id_ != txn->get_transaction_id()) {
                        if (it->granted_) {
                            return false;
                        }
                    } else {
                        cur = it;
                        break;
                    }
                }
                return true;
            });
            cur->granted_ = true;
            lock_request_queue.group_lock_mode_ = GroupLockMode::X;
            txn->get_lock_set()->emplace(lock_data_id);
            ul.release();
            return true;
        }
    }

    // 如果其他事务有其他锁，加锁失败（no-wait）
    if (lock_request_queue.group_lock_mode_ != GroupLockMode::NON_LOCK) {
        if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
            // Younger transaction requests the lock, abort the current transaction
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }

        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
        lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXCLUSIVE);
        std::unique_lock<std::mutex> ul(latch_, std::adopt_lock);
        auto &&cur = lock_request_queue.request_queue_.begin();
        // 通过条件：当前请求之前没有任何已授权的请求
        lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur]() {
            for (auto &&it = lock_request_queue.request_queue_.begin(); it != lock_request_queue.request_queue_.end();
                 ++it) {
                if (it->txn_id_ != txn->get_transaction_id()) {
                    if (it->granted_) {
                        return false;
                    }
                } else {
                    cur = it;
                    break;
                }
            }
            return true;
        });
        cur->granted_ = true;
        lock_request_queue.group_lock_mode_ = GroupLockMode::X;
        txn->get_lock_set()->emplace(lock_data_id);
        ul.release();
        return true;
    }

    if (txn->get_transaction_id() < lock_request_queue.oldest_txn_id_) {
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
    }

    // 将当前事务锁请求加到锁请求队列中
    lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXCLUSIVE, true);
    // 更新锁请求队列锁模式为排他锁
    lock_request_queue.group_lock_mode_ = GroupLockMode::X;
    // 添加行级排他锁
    txn->get_lock_set()->emplace(lock_data_id);
    return true;
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction *txn, int tab_fd) {
    std::lock_guard lock(latch_);

    if (!check_lock(txn)) {
        return false;
    }

    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &&it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) {
        it = lock_table_.emplace(std::piecewise_construct, std::forward_as_tuple(lock_data_id),
                                 std::forward_as_tuple()).first;
        it->second.oldest_txn_id_ = txn->get_transaction_id();
    }

    auto &lock_request_queue = it->second;
    for (auto &lock_request: lock_request_queue.request_queue_) {
        if (lock_request.txn_id_ == txn->get_transaction_id()) {
            // 已经有 S 锁或更高级别的锁，则申请成功
            if (lock_request.lock_mode_ == LockMode::SHARED ||
                lock_request.lock_mode_ == LockMode::S_IX ||
                lock_request.lock_mode_ == LockMode::EXCLUSIVE) {
                return true;
            }
            // 如果事务已经有意向读锁，升级读锁需要其他事务不持有写锁
            if (lock_request.lock_mode_ == LockMode::INTENTION_SHARED &&
                (lock_request_queue.group_lock_mode_ == GroupLockMode::S || lock_request_queue.group_lock_mode_ ==
                 GroupLockMode::IS)) {
                lock_request.lock_mode_ = LockMode::SHARED;
                lock_request_queue.group_lock_mode_ = GroupLockMode::S;
                ++lock_request_queue.shared_lock_num_;
                return true;
            }
            // 如果事务已经有 IX 锁，升级 S_IX 需要其他事务不持有 IX 锁
            if (lock_request.lock_mode_ == LockMode::INTENTION_EXCLUSIVE && lock_request_queue.IX_lock_num_ == 1) {
                lock_request.lock_mode_ = LockMode::S_IX;
                lock_request_queue.group_lock_mode_ = GroupLockMode::SIX;
                ++lock_request_queue.shared_lock_num_;
                return true;
            }

            if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
                // Younger transaction requests the lock, abort the current transaction
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
            lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::SHARED);
            std::unique_lock<std::mutex> ul(latch_, std::adopt_lock);
            auto &&cur = lock_request_queue.request_queue_.begin();
            lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur]() {
                for (auto &&it = lock_request_queue.request_queue_.begin();
                     it != lock_request_queue.request_queue_.end(); ++it) {
                    if (it->txn_id_ != txn->get_transaction_id()) {
                        if (it->lock_mode_ != LockMode::SHARED || it->granted_) {
                            return false;
                        }
                    } else {
                        cur = it;
                        break;
                    }
                }
                return true;
            });
            cur->granted_ = true;
            ++lock_request_queue.shared_lock_num_;
            // 锁合成
            if (lock_request_queue.group_lock_mode_ == GroupLockMode::IX) {
                lock_request_queue.group_lock_mode_ = GroupLockMode::SIX;
            } else {
                lock_request_queue.group_lock_mode_ = static_cast<GroupLockMode>(std::max(
                    static_cast<int>(GroupLockMode::S), static_cast<int>(lock_request_queue.group_lock_mode_)));
            }
            txn->get_lock_set()->emplace(lock_data_id);
            ul.release();
            return true;
        }
    }

    // 如果其他事务持有任意排他锁，加锁失败（no-wait）
    if (lock_request_queue.group_lock_mode_ == GroupLockMode::X ||
        lock_request_queue.group_lock_mode_ == GroupLockMode::IX ||
        lock_request_queue.group_lock_mode_ == GroupLockMode::SIX) {
        // Check for conflicting locks and apply wait-die logic
        if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
            // Younger transaction requests the lock, abort the current transaction
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
        lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::SHARED);
        std::unique_lock<std::mutex> ul(latch_, std::adopt_lock);
        auto &&cur = lock_request_queue.request_queue_.begin();
        lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur]() {
            for (auto &&it = lock_request_queue.request_queue_.begin(); it != lock_request_queue.request_queue_.end();
                 ++it) {
                if (it->txn_id_ != txn->get_transaction_id()) {
                    if (it->lock_mode_ != LockMode::SHARED || it->granted_) {
                        return false;
                    }
                } else {
                    cur = it;
                    break;
                }
            }
            return true;
        });
        cur->granted_ = true;
        ++lock_request_queue.shared_lock_num_;
        // 锁合成
        if (lock_request_queue.group_lock_mode_ == GroupLockMode::IX) {
            lock_request_queue.group_lock_mode_ = GroupLockMode::SIX;
        } else {
            lock_request_queue.group_lock_mode_ = static_cast<GroupLockMode>(std::max(
                static_cast<int>(GroupLockMode::S), static_cast<int>(lock_request_queue.group_lock_mode_)));
        }
        txn->get_lock_set()->emplace(lock_data_id);
        ul.release();
        return true;
    }

    if (txn->get_transaction_id() < lock_request_queue.oldest_txn_id_) {
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
    }

    // 将当前事务锁请求加到锁请求队列中
    lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::SHARED, true);
    // 更新锁请求队列锁模式为共享锁
    lock_request_queue.group_lock_mode_ = GroupLockMode::S;
    ++lock_request_queue.shared_lock_num_;
    // 添加表级共享锁
    txn->get_lock_set()->emplace(lock_data_id);
    return true;
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction *txn, int tab_fd) {
    std::lock_guard lock(latch_);

    if (!check_lock(txn)) {
        return false;
    }

    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &&it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) {
        it = lock_table_.emplace(std::piecewise_construct, std::forward_as_tuple(lock_data_id),
                                 std::forward_as_tuple()).first;
        it->second.oldest_txn_id_ = txn->get_transaction_id();
    }

    auto &lock_request_queue = it->second;
    for (auto &lock_request: lock_request_queue.request_queue_) {
        if (lock_request.txn_id_ == txn->get_transaction_id()) {
            // 已经有 X 锁，则申请成功
            if (lock_request.lock_mode_ == LockMode::EXCLUSIVE) {
                return true;
            }
            // 如果只存在一个事务，才能升级表写锁
            if (lock_request_queue.request_queue_.size() == 1) {
                lock_request.lock_mode_ = LockMode::EXCLUSIVE;
                lock_request_queue.group_lock_mode_ = GroupLockMode::X;
                return true;
            }
            // 判断回滚还是等待
            if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
                // Younger transaction requests the lock, abort the current transaction
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
            lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXCLUSIVE);
            std::unique_lock<std::mutex> ul(latch_, std::adopt_lock);
            auto &&cur = lock_request_queue.request_queue_.begin();
            // 当前请求前面没有任何已授权的请求
            lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur]() {
                for (auto &&it = lock_request_queue.request_queue_.begin();
                     it != lock_request_queue.request_queue_.end(); ++it) {
                    if (it->txn_id_ != txn->get_transaction_id()) {
                        if (it->granted_) {
                            return false;
                        }
                    } else {
                        cur = it;
                        break;
                    }
                }
                return true;
            });
            cur->granted_ = true;
            lock_request_queue.group_lock_mode_ = GroupLockMode::X;
            txn->get_lock_set()->emplace(lock_data_id);
            ul.release();
            return true;
        }
    }

    // 如果其他事务持有任意锁，加锁失败（no-wait）
    if (lock_request_queue.group_lock_mode_ != GroupLockMode::NON_LOCK) {
        // 判断回滚还是等待
        if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
            // Younger transaction requests the lock, abort the current transaction
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
        lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXCLUSIVE);
        std::unique_lock<std::mutex> ul(latch_, std::adopt_lock);
        auto &&cur = lock_request_queue.request_queue_.begin();
        // 当前请求前面没有任何已授权的请求
        lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur]() {
            for (auto &&it = lock_request_queue.request_queue_.begin(); it != lock_request_queue.request_queue_.end();
                 ++it) {
                if (it->txn_id_ != txn->get_transaction_id()) {
                    if (it->granted_) {
                        return false;
                    }
                } else {
                    cur = it;
                    break;
                }
            }
            return true;
        });
        cur->granted_ = true;
        lock_request_queue.group_lock_mode_ = GroupLockMode::X;
        txn->get_lock_set()->emplace(lock_data_id);
        ul.release();
        return true;
    }

    if (txn->get_transaction_id() < lock_request_queue.oldest_txn_id_) {
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
    }

    // 将当前事务锁请求加到锁请求队列中
    lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXCLUSIVE, true);
    // 更新锁请求队列锁模式为排他锁
    lock_request_queue.group_lock_mode_ = GroupLockMode::X;
    // 添加表级排他锁
    txn->get_lock_set()->emplace(lock_data_id);
    return true;
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction *txn, int tab_fd) {
    std::lock_guard lock(latch_);

    if (!check_lock(txn)) {
        return false;
    }

    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &&it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) {
        it = lock_table_.emplace(std::piecewise_construct, std::forward_as_tuple(lock_data_id),
                                 std::forward_as_tuple()).first;
        it->second.oldest_txn_id_ = txn->get_transaction_id();
    }

    auto &lock_request_queue = it->second;
    for (auto &lock_request: lock_request_queue.request_queue_) {
        // 没有锁比 IS 更低级，请求队列中事务存在则加锁成功
        if (lock_request.txn_id_ == txn->get_transaction_id()) {
            assert(lock_request.granted_);
            return true;
        }
    }

    // 如果其他事务持有 X 锁，加锁失败（no-wait）
    if (lock_request_queue.group_lock_mode_ == GroupLockMode::X) {
        // 判断回滚还是等待
        if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
            // Younger transaction requests the lock, abort the current transaction
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
        lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::INTENTION_SHARED);
        std::unique_lock<std::mutex> ul(latch_, std::adopt_lock);
        auto &&cur = lock_request_queue.request_queue_.begin();
        // 当前请求前面没有任何已授权的请求
        lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur]() {
            for (auto &&it = lock_request_queue.request_queue_.begin(); it != lock_request_queue.request_queue_.end();
                 ++it) {
                if (it->txn_id_ != txn->get_transaction_id()) {
                    if (it->lock_mode_ != LockMode::SHARED || it->granted_) {
                        return false;
                    }
                } else {
                    cur = it;
                    break;
                }
            }
            return true;
        });
        cur->granted_ = true;

        // 只有队列没有锁才能设置为 IS 锁
        if (lock_request_queue.group_lock_mode_ == GroupLockMode::NON_LOCK) {
            lock_request_queue.group_lock_mode_ = GroupLockMode::IS;
        }

        txn->get_lock_set()->emplace(lock_data_id);
        ul.release();
        return true;
    }

    // 只有队列没有锁才能设置为 IS 锁
    if (lock_request_queue.group_lock_mode_ == GroupLockMode::NON_LOCK) {
        lock_request_queue.group_lock_mode_ = GroupLockMode::IS;
    }

    if (txn->get_transaction_id() < lock_request_queue.oldest_txn_id_) {
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
    }

    // 将当前事务锁请求加到锁请求队列中
    lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::INTENTION_SHARED, true);
    // 添加表级 IS 锁
    txn->get_lock_set()->emplace(lock_data_id);
    return true;
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction *txn, int tab_fd) {
    std::lock_guard lock(latch_);

    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &&it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) {
        it = lock_table_.emplace(std::piecewise_construct, std::forward_as_tuple(lock_data_id),
                                 std::forward_as_tuple()).first;
        it->second.oldest_txn_id_ = txn->get_transaction_id();
    }

    auto &lock_request_queue = it->second;
    for (auto &lock_request: lock_request_queue.request_queue_) {
        if (lock_request.txn_id_ == txn->get_transaction_id()) {
            // 已经有任意排它锁
            if (lock_request.lock_mode_ == LockMode::S_IX ||
                lock_request.lock_mode_ == LockMode::INTENTION_EXCLUSIVE ||
                lock_request.lock_mode_ == LockMode::EXCLUSIVE) {
                return true;
            }
            // 只有一个事务在读时，才能升级为 S_IX 锁
            if (lock_request.lock_mode_ == LockMode::SHARED && lock_request_queue.shared_lock_num_ == 1) {
                ++lock_request_queue.IX_lock_num_;
                lock_request.lock_mode_ = LockMode::S_IX;
                lock_request_queue.group_lock_mode_ = GroupLockMode::SIX;
                return true;
            }
            // 当前事务持有 IS 锁，且请求队列中没有比 IX 更高级的锁，才能升级为 IX 锁
            if (lock_request.lock_mode_ == LockMode::INTENTION_SHARED &&
                (lock_request_queue.group_lock_mode_ == GroupLockMode::IS ||
                 lock_request_queue.group_lock_mode_ == GroupLockMode::IX)) {
                ++lock_request_queue.IX_lock_num_;
                lock_request.lock_mode_ = LockMode::INTENTION_EXCLUSIVE;
                lock_request_queue.group_lock_mode_ = GroupLockMode::IX;
                return true;
            }

            // 判断回滚还是等待
            if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
                // Younger transaction requests the lock, abort the current transaction
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
            lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::INTENTION_EXCLUSIVE);
            std::unique_lock<std::mutex> ul(latch_, std::adopt_lock);
            auto &&cur = lock_request_queue.request_queue_.begin();
            // 当前请求前面没有任何已授权的请求
            lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur]() {
                for (auto &&it = lock_request_queue.request_queue_.begin();
                     it != lock_request_queue.request_queue_.end(); ++it) {
                    if (it->txn_id_ != txn->get_transaction_id()) {
                        if (it->granted_) {
                            return false;
                        }
                    } else {
                        cur = it;
                        break;
                    }
                }
                return true;
            });
            cur->granted_ = true;
            ++lock_request_queue.IX_lock_num_;
            // 合成一下
            if (lock_request_queue.group_lock_mode_ == GroupLockMode::S) {
                lock_request_queue.group_lock_mode_ = GroupLockMode::SIX;
            } else {
                lock_request_queue.group_lock_mode_ = static_cast<GroupLockMode>(std::max(
                    static_cast<int>(GroupLockMode::IX), static_cast<int>(lock_request_queue.group_lock_mode_)));
            }
            txn->get_lock_set()->emplace(lock_data_id);
            ul.release();
            return true;
        }
    }

    // 如果其他事务持有共享锁或最高级别的排他锁，加锁失败（no-wait）
    if (lock_request_queue.group_lock_mode_ == GroupLockMode::X ||
        lock_request_queue.group_lock_mode_ == GroupLockMode::S ||
        lock_request_queue.group_lock_mode_ == GroupLockMode::SIX) {
        // 判断回滚还是等待
        if (txn->get_transaction_id() > lock_request_queue.oldest_txn_id_) {
            // Younger transaction requests the lock, abort the current transaction
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
        lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::INTENTION_EXCLUSIVE);
        std::unique_lock<std::mutex> ul(latch_, std::adopt_lock);
        auto &&cur = lock_request_queue.request_queue_.begin();
        // 当前请求前面没有任何已授权的请求
        lock_request_queue.cv_.wait(ul, [&lock_request_queue, txn, &cur]() {
            for (auto &&it = lock_request_queue.request_queue_.begin(); it != lock_request_queue.request_queue_.end();
                 ++it) {
                if (it->txn_id_ != txn->get_transaction_id()) {
                    if (it->granted_) {
                        return false;
                    }
                } else {
                    cur = it;
                    break;
                }
            }
            return true;
        });
        cur->granted_ = true;
        ++lock_request_queue.IX_lock_num_;
        // 合成一下
        if (lock_request_queue.group_lock_mode_ == GroupLockMode::S) {
            lock_request_queue.group_lock_mode_ = GroupLockMode::SIX;
        } else {
            lock_request_queue.group_lock_mode_ = static_cast<GroupLockMode>(std::max(
                static_cast<int>(GroupLockMode::IX), static_cast<int>(lock_request_queue.group_lock_mode_)));
        }
        txn->get_lock_set()->emplace(lock_data_id);
        ul.release();
        return true;
    }

    if (txn->get_transaction_id() < lock_request_queue.oldest_txn_id_) {
        lock_request_queue.oldest_txn_id_ = txn->get_transaction_id();
    }

    ++lock_request_queue.IX_lock_num_;
    // 将当前事务锁请求加到锁请求队列中
    lock_request_queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::INTENTION_EXCLUSIVE, true);
    lock_request_queue.group_lock_mode_ = GroupLockMode::IX;
    // 添加表级 IX 锁
    txn->get_lock_set()->emplace(lock_data_id);
    return true;
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction *txn, const LockDataId &lock_data_id) {
    std::lock_guard lock(latch_);

    auto &txn_state = txn->get_state();
    // 事务结束，不能再解锁
    if (txn_state == TransactionState::COMMITTED || txn_state == TransactionState::ABORTED) {
        return false;
    }

    if (txn_state == TransactionState::GROWING) {
        txn_state = TransactionState::SHRINKING;
    }

    std::unordered_map<LockDataId, LockRequestQueue>::iterator it;
    std::unordered_map<IndexMeta, std::unordered_map<LockDataId, LockRequestQueue> >::iterator ii;

    if (lock_data_id.type_ == LockDataType::GAP) {
        ii = gap_lock_table_.find(lock_data_id.index_meta_);
        if (ii == gap_lock_table_.end()) {
            return true;
        }
        it = ii->second.find(lock_data_id);
        if (it == ii->second.end()) {
            return true;
        }
    } else {
        it = lock_table_.find(lock_data_id);
        if (it == lock_table_.end()) {
            return true;
        }
    }

    auto &lock_request_queue = it->second;
    auto &request_queue = lock_request_queue.request_queue_;

    auto &&request = request_queue.begin();
    for (; request != request_queue.end(); ++request) {
        if (request->txn_id_ == txn->get_transaction_id()) {
            break;
        }
    }

    if (request == request_queue.end()) {
        return true;
    }

    // 一个事务可能对某个记录持有多个锁，S，IX
    do {
        // 维护锁请求队列
        if (request->lock_mode_ == LockMode::SHARED || request->lock_mode_ == LockMode::S_IX) {
            --lock_request_queue.shared_lock_num_;
        }
        if (request->lock_mode_ == LockMode::INTENTION_EXCLUSIVE || request->lock_mode_ == LockMode::S_IX) {
            --lock_request_queue.IX_lock_num_;
        }
        // 删除该锁请求
        request_queue.erase(request);

        request = request_queue.begin();
        for (; request != request_queue.end(); ++request) {
            if (request->txn_id_ == txn->get_transaction_id()) {
                break;
            }
        }
    } while (request != request_queue.end());

    // 维护队列锁模式，为空则无锁
    // TODO 擦除锁表
    if (request_queue.empty()) {
        lock_request_queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
        lock_request_queue.oldest_txn_id_ = INT32_MAX;
        // 唤醒等待的事务
        lock_request_queue.cv_.notify_all();

        if (lock_data_id.type_ == LockDataType::GAP) {
            // 相交的间隙锁也得唤醒
            for (auto &[data_id, queue]: ii->second) {
                // if (queue.group_lock_mode_ != GroupLockMode::NON_LOCK) {
                if (lock_data_id.gap_.isCoincide(data_id.gap_)) {
                    queue.cv_.notify_all();
                }
                // }
            }
        }

        return true;
    }

    // 否则找到级别最高的锁和时间戳最小的事务
    auto max_lock_mode = LockMode::INTENTION_SHARED;
    for (auto &request: request_queue) {
        max_lock_mode = std::max(max_lock_mode, request.lock_mode_);
        if (request.txn_id_ < lock_request_queue.oldest_txn_id_) {
            lock_request_queue.oldest_txn_id_ = request.txn_id_;
        }
    }

    lock_request_queue.group_lock_mode_ = static_cast<GroupLockMode>(static_cast<int>(max_lock_mode) + 1);

    // 唤醒等待的事务
    lock_request_queue.cv_.notify_all();

    if (lock_data_id.type_ == LockDataType::GAP) {
        // 相交的锁表也得唤醒
        for (auto &[data_id, queue]: ii->second) {
            // if (queue.group_lock_mode_ != GroupLockMode::NON_LOCK) {
            if (lock_data_id.gap_.isCoincide(data_id.gap_)) {
                queue.cv_.notify_all();
            }
            // }
        }
    }
    return true;
}
