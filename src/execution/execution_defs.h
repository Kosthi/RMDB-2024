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

#include "transaction/transaction.h"
#include "transaction/transaction_manager.h"

#include "defs.h"
#include "errors.h"

#include "system/sm_meta.h"
#include "common/common.h"

struct CondOp {
    CompOp op = OP_INVALID;
    Value rhs_val;
    int offset = 0;

    explicit CondOp(int offset_) : rhs_val(), offset(offset_) {
    }
};

static inline int compare(const char *a, const char *b, int col_len, ColType col_type) {
    switch (col_type) {
        case TYPE_INT: {
            const int ai = *reinterpret_cast<const int *>(a);
            const int bi = *reinterpret_cast<const int *>(b);
            return (ai > bi) - (ai < bi);
        }
        case TYPE_FLOAT: {
            const float af = *reinterpret_cast<const float *>(a);
            const float bf = *reinterpret_cast<const float *>(b);
            return (af > bf) - (af < bf);
        }
        case TYPE_STRING:
            return memcmp(a, b, col_len);
        default:
            throw InternalError("Unexpected data type！");
    }
}

inline int ix_compare(char *&a, char *&b, const IndexMeta &index_meta) {
    for (auto &[index_offset, col_meta]: index_meta.cols) {
        int res = compare(a + index_offset, b + index_offset, col_meta.len, col_meta.type);
        if (res != 0) return res;
    }
    return 0;
}

static inline void add(char *a, const char *b, ColType col_type) {
    switch (col_type) {
        case TYPE_INT: {
            const int ai = *reinterpret_cast<const int *>(a);
            const int bi = *reinterpret_cast<const int *>(b);
            const int res = ai + bi;
            memcpy(a, &res, sizeof(int));
            break;
        }
        case TYPE_FLOAT: {
            const float af = *reinterpret_cast<const float *>(a);
            const float bf = *reinterpret_cast<const float *>(b);
            const float res = af + bf;
            memcpy(a, &res, sizeof(float));
            break;
        }
        case TYPE_STRING:
        default:
            throw InternalError("Unexpected data type to add！");
    }
}

// MVCC Support
auto ReconstructTuple(const TabMeta &tab_meta, const RmRecord &base_tuple,
                      const std::vector<UndoLog> &undo_logs) -> std::optional<RmRecord> {
    // 设计的想法：unlog中直接存储对tuple的快照，这样就可以直接赋值
    // 为了省出空间，目前的设计tuple只存储了部分，这样只能采取逐级回滚的方法
    char *data = new char[base_tuple.size];
    memcpy(data, base_tuple.data, base_tuple.size);

    int offset = 0;
    bool is_delete = false;
    for (const auto &undo_log: undo_logs) {
        if (undo_log.is_deleted_) {
            is_delete = true;
        } else {;
            is_delete = false;
            for (std::size_t i = 0; i < undo_log.modified_fields_.size(); ++i) {
                if (undo_log.modified_fields_[i]) {
                    memcpy(data + tab_meta.cols[i].offset, undo_log.tuple_.data + offset, tab_meta.cols[i].len);
                    offset += tab_meta.cols[i].len;
                }
            }
            // auto &&key_schema = Schema::CopySchema(schema, attrs);
            // for (std::size_t i = 0, j = 0; i < undo_log.modified_fields_.size(); ++i) {
            //     if (undo_log.modified_fields_[i]) {
            //         // values[i] = undo_log.tuple_.GetValue(&key_schema, j++);
            //     }
            // }
        }
    }

    return is_delete ? std::nullopt : std::make_optional<RmRecord>(data, base_tuple.size, true);
}

// void TxnMgrDbg(const std::string &info, TransactionManager *txn_mgr, const TableInfo *table_info,
//                TableHeap *table_heap) {
// always use stderr for printing logs...
// fmt::println(stderr, "debug_hook: {}", info);
//
// fmt::println(
//     stderr,
//     "You see this line of text because you have not implemented `TxnMgrDbg`. You should do this once you have "
//     "finished task 2. Implementing this helper function will save you a lot of time for debugging in later tasks.");

// We recommend implementing this function as traversing the table heap and print the version chain. An example output
// of our reference solution:
//
// debug_hook: before verify scan
// RID=0/0 ts=txn8 tuple=(1, <NULL>, <NULL>)
//   txn8@0 (2, _, _) ts=1
// RID=0/1 ts=3 tuple=(3, <NULL>, <NULL>)
//   txn5@0 <del> ts=2
//   txn3@0 (4, <NULL>, <NULL>) ts=1
// RID=0/2 ts=4 <del marker> tuple=(<NULL>, <NULL>, <NULL>)
//   txn7@0 (5, <NULL>, <NULL>) ts=3
// RID=0/3 ts=txn6 <del marker> tuple=(<NULL>, <NULL>, <NULL>)
//   txn6@0 (6, <NULL>, <NULL>) ts=2
//   txn3@1 (7, _, _) ts=1
// }
