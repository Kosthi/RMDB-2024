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

#include <cassert>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <parser/ast.h>

#include "defs.h"
#include "record/rm_defs.h"

class Plan;
// 这里使用前置声明避免头文件互相包含 common.h - analyze.h
class Query;
class AbstractExecutor;

struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }

    bool operator==(const TabCol &y) const {
        return tab_name == y.tab_name && col_name == y.col_name;
    }
};

struct Value {
    ColType type; // type of value
    union {
        int int_val; // int value
        float float_val; // float value
    };

    std::string str_val; // string value

    std::shared_ptr<RmRecord> raw; // raw record buffer

    Value() = default;

    // 拷贝构造
    Value(const Value &other) noexcept
        : type(other.type), raw(other.raw) {
        if (type == TYPE_INT) {
            int_val = other.int_val;
        } else if (type == TYPE_FLOAT) {
            float_val = other.float_val;
        } else if (type == TYPE_STRING) {
            str_val = other.str_val;
        }
    }

    // 移动构造
    Value(Value &&other) noexcept
        : type(other.type), raw(std::move(other.raw)) {
        if (type == TYPE_INT) {
            int_val = other.int_val;
            other.int_val = 0;
        } else if (type == TYPE_FLOAT) {
            float_val = other.float_val;
            other.float_val = 0.0f;
        } else if (type == TYPE_STRING) {
            str_val = std::move(other.str_val);
        }
    }

    // 拷贝赋值构造
    Value &operator=(const Value &other) noexcept {
        if (this != &other) {
            type = other.type;
            if (type == TYPE_INT) {
                int_val = other.int_val;
            } else if (type == TYPE_FLOAT) {
                float_val = other.float_val;
            } else if (type == TYPE_STRING) {
                str_val = other.str_val;
            }
            raw = other.raw;
        }
        return *this;
    }

    // 移动赋值构造
    Value &operator=(Value &&other) noexcept {
        if (this != &other) {
            type = other.type;
            if (type == TYPE_INT) {
                int_val = other.int_val;
                other.int_val = 0;
            } else if (type == TYPE_FLOAT) {
                float_val = other.float_val;
                other.float_val = 0.0f;
            } else if (type == TYPE_STRING) {
                str_val = std::move(other.str_val);
            }
            raw = std::move(other.raw);
        }
        return *this;
    }

    ~Value() = default;

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *) (raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *) (raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int) str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }

    bool operator==(const Value &other) const {
        if (type != other.type) return false;
        switch (type) {
            case TYPE_INT:
                return int_val == other.int_val;
            case TYPE_FLOAT:
                return float_val == other.float_val;
            case TYPE_STRING:
                return str_val == other.str_val;
            default:
                return false;
        }
    }
};

enum CompOp { OP_INVALID, OP_IN, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };

struct Condition {
    AggType agg_type;
    TabCol lhs_col; // left-hand side column
    CompOp op; // comparison operator
    bool is_rhs_val; // true if right-hand side is a value (not a column)
    bool is_sub_query; // 是否是子查询
    std::shared_ptr<Query> sub_query; // 子查询
    std::shared_ptr<Plan> sub_query_plan; // 子查询计划
    std::shared_ptr<AbstractExecutor> prev; // 子查询算子
    TabCol rhs_col; // right-hand side column
    Value rhs_val; // right-hand side value
    std::vector<Value> rhs_value_list; // 值列表

    // Condition() noexcept = default;
    //
    // Condition(const Condition&) noexcept = default;
    //
    // Condition(Condition&& other) noexcept
    // : agg_type(other.agg_type),
    //   lhs_col(std::move(other.lhs_col)),
    //   op(other.op),
    //   is_rhs_val(other.is_rhs_val),
    //   is_sub_query(other.is_sub_query),
    //   sub_query(std::move(other.sub_query)),
    //   sub_query_plan(std::move(other.sub_query_plan)),
    //   prev(std::move(other.prev)),
    //   rhs_col(std::move(other.rhs_col)),
    //   rhs_val(std::move(other.rhs_val)),
    //   rhs_value_list(std::move(other.rhs_value_list)) {
    // }
    //
    // Condition& operator=(Condition&& other) noexcept {
    //     if (this != &other) {  // 防止自赋值
    //         agg_type = other.agg_type;
    //         lhs_col = std::move(other.lhs_col);
    //         op = other.op;
    //         is_rhs_val = other.is_rhs_val;
    //         is_sub_query = other.is_sub_query;
    //         sub_query = std::move(other.sub_query);
    //         sub_query_plan = std::move(other.sub_query_plan);
    //         prev = std::move(other.prev);
    //         rhs_col = std::move(other.rhs_col);
    //         rhs_val = std::move(other.rhs_val);
    //         rhs_value_list = std::move(other.rhs_value_list);
    //     }
    //     return *this;
    // }
};

struct SetClause {
    TabCol lhs;
    Value rhs;
    // 是否是 v=v+1
    bool is_incr = false;

    SetClause() noexcept = default;

    SetClause(TabCol lhs_, Value rhs_, bool is_incr_) :
    lhs(std::move(lhs_)), rhs(std::move(rhs_)), is_incr(is_incr_) {}
};

// Utility function to combine hashes
template<typename T>
inline void hash_combine(std::size_t &seed, const T &val) {
    seed ^= std::hash<T>()(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

// Custom hash function for Value
namespace std {
    template<>
    struct hash<Value> {
        std::size_t operator()(const Value &v) const noexcept {
            std::size_t seed = 0;
            hash_combine(seed, v.type);
            switch (v.type) {
                case TYPE_INT: {
                    hash_combine(seed, v.int_val);
                    break;
                }
                case TYPE_FLOAT: {
                    hash_combine(seed, v.float_val);
                    break;
                }
                case TYPE_STRING: {
                    hash_combine(seed, v.str_val);
                    break;
                }
                // default:
                //     throw InternalError("Unexpected data type！");
            }
            return seed;
        }
    };

    template<>
    struct hash<TabCol> {
        std::size_t operator()(const TabCol &tc) const noexcept {
            std::size_t h1 = std::hash<std::string>{}(tc.tab_name);
            std::size_t h2 = std::hash<std::string>{}(tc.col_name);
            // 结合两个哈希值，使用位运算符来减少冲突
            return h1 ^ (h2 << 1);
        }
    };
}
