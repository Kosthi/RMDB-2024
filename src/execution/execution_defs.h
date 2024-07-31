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

    // 默认构造函数
    CondOp() = default;

    // 有参构造函数
    explicit CondOp(int offset_) : rhs_val(), offset(offset_) {
    }

    // 拷贝构造函数
    CondOp(const CondOp &other) = default;

    // 移动构造函数
    CondOp(CondOp &&other) noexcept : op(other.op), rhs_val(std::move(other.rhs_val)), offset(other.offset) {
        other.op = OP_INVALID;
        other.offset = 0;
    }

    // 拷贝赋值运算符
    CondOp &operator=(const CondOp &other) noexcept {
        if (this != &other) {
            op = other.op;
            rhs_val = other.rhs_val;
            offset = other.offset;
        }
        return *this;
    }

    // 移动赋值运算符
    CondOp &operator=(CondOp &&other) noexcept {
        if (this != &other) {
            op = other.op;
            rhs_val = std::move(other.rhs_val);
            offset = other.offset;
            other.op = OP_INVALID;
            other.offset = 0;
        }
        return *this;
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
