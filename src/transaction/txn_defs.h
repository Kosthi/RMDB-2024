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
#include <optional>
#include <utility>
#include "common/common.h"
#include "execution/execution_defs.h"
#include "system/sm_meta.h"

#include "common/config.h"
#include "defs.h"
#include "record/rm_defs.h"

/* 标识事务状态 */
enum class TransactionState { DEFAULT, GROWING, SHRINKING, COMMITTED, ABORTED };

/* 系统的隔离级别，当前赛题中为可串行化隔离级别 */
enum class IsolationLevel { READ_UNCOMMITTED, REPEATABLE_READ, READ_COMMITTED, SERIALIZABLE };

/* 事务写操作类型，包括插入、删除、更新三种操作 */
enum class WType { INSERT_TUPLE = 0, DELETE_TUPLE, UPDATE_TUPLE };

/**
 * @brief 事务的写操作记录，用于事务的回滚
 * INSERT
 * --------------------------------
 * | wtype | tab_name | tuple_rid |
 * --------------------------------
 * DELETE / UPDATE
 * ----------------------------------------------
 * | wtype | tab_name | tuple_rid | tuple_value |
 * ----------------------------------------------
 */
class WriteRecord {
public:
    WriteRecord() = default;

    // constructor for insert operation
    WriteRecord(WType wtype, const Rid &rid, const RmRecord &record, std::string tab_name)
        : wtype_(wtype), tab_name_(std::move(tab_name)), rid_(rid), record_(record) {
    }

    // constructor for delete operation
    WriteRecord(WType wtype, std::string tab_name, const Rid &rid, const RmRecord &record)
        : wtype_(wtype), tab_name_(std::move(tab_name)), rid_(rid), record_(record) {
    }

    // constructor for update operation
    WriteRecord(WType wtype, std::string tab_name, const Rid &rid, const RmRecord &old_record,
                const RmRecord &new_record)
        : wtype_(wtype), tab_name_(std::move(tab_name)), rid_(rid), record_(old_record), updated_record_(new_record) {
    }

    ~WriteRecord() = default;

    inline RmRecord &GetRecord() { return record_; }

    inline RmRecord &GetUpdatedRecord() { return updated_record_; }

    inline Rid &GetRid() { return rid_; }

    inline WType &GetWriteType() { return wtype_; }

    inline std::string &GetTableName() { return tab_name_; }

private:
    WType wtype_;
    std::string tab_name_;
    Rid rid_;
    RmRecord record_;
    RmRecord updated_record_;
};

/* 多粒度锁，加锁对象的类型，包括记录、表和间隙 */
enum class LockDataType { TABLE = 0, RECORD = 1, GAP = 2 };

class Gap {
public:
    Gap() = default;

    explicit Gap(std::vector<std::pair<CondOp, CondOp> > &index_conds) {
        index_conds_ = index_conds;
    }

    bool isInGap(const RmRecord &rec) const {
        return cmpIndexLeftConds(rec) && cmpIndexRightConds(rec);
    }

    // 结论：A的最小值小于等于B的最大值，并且B的最小值小于等于A的最大值，那么他们就是相交的。(Amin<=Bmax)&&(Amax>=Bmin)
    bool isCoincide(const Gap &gap) const {
        for (int i = 0; i < index_conds_.size(); ++i) {
            auto &lhs_cond = index_conds_[i].first;
            auto &rhs_cond = gap.index_conds_[i].second;
            if (lhs_cond.op != OP_INVALID && rhs_cond.op != OP_INVALID) {
                auto &type = lhs_cond.rhs_val.type;
                auto &lhs_rec = lhs_cond.rhs_val.raw;
                auto &rhs_rec = rhs_cond.rhs_val.raw;
                int cmp = compare(lhs_rec->data, rhs_rec->data, lhs_rec->size, type);
                // 1.1 > 5 < 5 不相交
                // 1.2 > 5 = 5 不相交
                // 1.3 > 5 <= 5 不相交

                // 2.1 >= 5 < 5 不相交
                // 2.2 >= 5 = 5 相交
                // 2.3 >= 5 <= 5 相交

                // 3.1 = 5 < 5 不相交
                // 3.2 = 5 = 5 相交
                // 3.3 = 5 <= 5 相交
                // (OP_GE || OP_EQ) && (OP_LE || OP_EQ) -> (!OP_GE && !OP_EQ) || (!OP_LE && !OP_EQ) -> (OP_GT) || (OP_LT)
                if (cmp > 0 || (cmp == 0 && (lhs_cond.op == OP_GT || rhs_cond.op == OP_LT))) {
                    return false;
                }
            }
        }

        for (int i = 0; i < index_conds_.size(); ++i) {
            auto &lhs_cond = gap.index_conds_[i].first;
            auto &rhs_cond = index_conds_[i].second;
            if (lhs_cond.op != OP_INVALID && rhs_cond.op != OP_INVALID) {
                auto &type = lhs_cond.rhs_val.type;
                auto &lhs_rec = lhs_cond.rhs_val.raw;
                auto &rhs_rec = rhs_cond.rhs_val.raw;
                int cmp = compare(lhs_rec->data, rhs_rec->data, lhs_rec->size, type);
                // 1.1 > 5 < 5 不相交
                // 1.2 > 5 = 5 不相交
                // 1.3 > 5 <= 5 不相交

                // 2.1 >= 5 < 5 不相交
                // 2.2 >= 5 = 5 相交
                // 2.3 >= 5 <= 5 相交

                // 3.1 = 5 < 5 不相交
                // 3.2 = 5 = 5 相交
                // 3.3 = 5 <= 5 相交
                // !(OP_GE || OP_EQ) && (OP_LE || OP_EQ) -> (!OP_GE && !OP_EQ) || (!OP_LE && !OP_EQ) -> (OP_GT) || (OP_LT)
                if (cmp > 0 || (cmp == 0 && (lhs_cond.op == OP_GT || rhs_cond.op == OP_LT))) {
                    return false;
                }
            }
        }

        return true;
    }

    bool cmpIndexLeftConds(const RmRecord &rec) const {
        for (auto &[cond, _]: index_conds_) {
            if (cond.op != OP_INVALID && !cmpIndexCond(rec, cond)) {
                return false;
            }
        }
        return true;
    }

    bool cmpIndexRightConds(const RmRecord &rec) const {
        for (auto &[_, cond]: index_conds_) {
            if (cond.op != OP_INVALID && !cmpIndexCond(rec, cond)) {
                return false;
            }
        }
        return true;
    }

    static bool cmpIndexCond(const RmRecord &rec, const CondOp &cond) {
        int cmp = compare(rec.data + cond.offset, cond.rhs_val.raw->data, cond.rhs_val.raw->size, cond.rhs_val.type);
        switch (cond.op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
            default:
                throw InternalError("Unexpected op type！");
        }
    }

    std::vector<std::pair<CondOp, CondOp> > index_conds_;
};

// Hash function for CondOp
namespace std {
    template<>
    struct hash<CondOp> {
        std::size_t operator()(const CondOp &cond) const noexcept {
            std::size_t hash = std::hash<int>()(cond.op);
            hash ^= std::hash<Value>()(cond.rhs_val) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<int>()(cond.offset) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
}

// Hash function for std::pair<CondOp, CondOp>
namespace std {
    template<>
    struct hash<std::pair<CondOp, CondOp> > {
        std::size_t operator()(const std::pair<CondOp, CondOp> &pair) const {
            std::size_t hash = std::hash<CondOp>()(pair.first);
            hash ^= std::hash<CondOp>()(pair.second) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
}

// Hash function for Gap
namespace std {
    template<>
    struct hash<Gap> {
        std::size_t operator()(const Gap &gap) const {
            std::size_t hash = 0;
            for (const auto &cond_pair: gap.index_conds_) {
                hash ^= std::hash<std::pair<CondOp, CondOp> >()(cond_pair) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };
}

/**
 * @description: 加锁对象的唯一标识
 */
class LockDataId {
public:
    /* 表级锁 */
    LockDataId(int fd, LockDataType type) {
        assert(type == LockDataType::TABLE);
        fd_ = fd;
        type_ = type;
        rid_.page_no = -1;
        rid_.slot_no = -1;
    }

    /* 行级锁 */
    LockDataId(int fd, const Rid &rid, LockDataType type) {
        assert(type == LockDataType::RECORD);
        fd_ = fd;
        rid_ = rid;
        type_ = type;
    }

    /* 间隙锁 */
    // 左开右开，如果有相等的情况边界用行锁锁住
    // LockDataId(int fd, IndexMeta &index_meta, std::optional<RmRecord>& lower, std::optional<RmRecord>& upper, LockDataType type) {
    //     assert(type == LockDataType::GAP);
    //     fd_ = fd;
    //     index_meta_ = index_meta;
    //     lower_ = std::move(lower);
    //     upper_ = std::move(upper);
    //     type_ = type;
    // }

    /* 最细粒度间隙锁 */
    // 查询条件即为锁范围
    LockDataId(int fd, IndexMeta &index_meta, Gap &gap, LockDataType type) {
        assert(type == LockDataType::GAP);
        fd_ = fd;
        index_meta_ = index_meta;
        gap_ = std::move(gap);
        type_ = type;
    }

    inline int64_t Get() const {
        if (type_ == LockDataType::TABLE) {
            // fd_
            return static_cast<int64_t>(fd_);
        }
        if (type_ == LockDataType::RECORD) {
            // fd_, rid_.page_no, rid.slot_no
            return ((static_cast<int64_t>(type_)) << 63) | ((static_cast<int64_t>(fd_)) << 31) |
                   ((static_cast<int64_t>(rid_.page_no)) << 16) | rid_.slot_no;
        }
        return ((static_cast<int64_t>(type_)) << 63) | (static_cast<int64_t>(fd_) << 31) |
               (static_cast<int64_t>(gap_hasher_(gap_)));
    }

    bool operator==(const LockDataId &other) const {
        if (type_ != other.type_) return false;
        if (fd_ != other.fd_) return false;
        if (type_ == LockDataType::RECORD) {
            return rid_ == other.rid_;
        }
        return index_meta_ == other.index_meta_ && gap_hasher_(gap_) == gap_hasher_(other.gap_);
    }

    int fd_;
    Rid rid_;
    IndexMeta index_meta_; // 间隙锁，锁住的索引
    Gap gap_; // 间隙
    std::hash<Gap> gap_hasher_;
    LockDataType type_;
};

template<>
struct std::hash<LockDataId> {
    size_t operator()(const LockDataId &obj) const { return std::hash<int64_t>()(obj.Get()); }
};

/* 事务回滚原因 */
enum class AbortReason { LOCK_ON_SHIRINKING = 0, UPGRADE_CONFLICT, DEADLOCK_PREVENTION };

/* 事务回滚异常，在rmdb.cpp中进行处理 */
class TransactionAbortException : public std::exception {
    txn_id_t txn_id_;
    AbortReason abort_reason_;

public:
    explicit TransactionAbortException(txn_id_t txn_id, AbortReason abort_reason)
        : txn_id_(txn_id), abort_reason_(abort_reason) {
    }

    txn_id_t get_transaction_id() { return txn_id_; }
    AbortReason GetAbortReason() { return abort_reason_; }

    std::string GetInfo() {
        switch (abort_reason_) {
            case AbortReason::LOCK_ON_SHIRINKING: {
                return "Transaction " + std::to_string(txn_id_) +
                       " aborted because it cannot request locks on SHRINKING phase\n";
            }
            case AbortReason::UPGRADE_CONFLICT: {
                return "Transaction " + std::to_string(txn_id_) +
                       " aborted because another transaction is waiting for upgrading\n";
            }
            case AbortReason::DEADLOCK_PREVENTION: {
                return "Transaction " + std::to_string(txn_id_) + " aborted for deadlock prevention\n";
            }
            default: {
                return "Transaction aborted\n";
            }
        }
    }
};
