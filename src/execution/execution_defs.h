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
