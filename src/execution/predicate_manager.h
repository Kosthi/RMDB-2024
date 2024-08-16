//
// Created by Koschei on 2024/7/20.
//

#pragma once

// #include "execution_defs.h"

// 解析列的谓词信息 first >, second <
class PredicateManager {
public:
    PredicateManager() = default;

    // TODO 暂时仅支持索引列解析
    explicit PredicateManager(IndexMeta &index_meta) {
        predicates_.reserve(index_meta.col_num);
        index_conds_.reserve(index_meta.col_num);
        for (size_t i = 0; i < index_meta.cols.size(); ++i) {
            predicates_.emplace(index_meta.cols[i].second.name, i);
            index_conds_.emplace_back(index_meta.cols[i].first,
                                      index_meta.cols[i].first);
        }
    }

    bool addPredicate(const std::string &column, Condition &cond) {
        // 非索引字段
        if (predicates_.count(column) == 0) {
            return false;
        }
        // 左边
        if (cond.op == OP_GT || cond.op == OP_GE || cond.op == OP_EQ) {
            insertLeft(column, cond);
        }
        // 右边
        if (cond.op == OP_LT || cond.op == OP_LE || cond.op == OP_EQ) {
            insertRight(column, cond);
        }
        return true;
    }

    void insertLeft(const std::string &column, Condition &cond) {
        index_conds_[predicates_[column]].first.op = cond.op;
        index_conds_[predicates_[column]].first.rhs_val = cond.rhs_val;
    }

    void insertRight(const std::string &column, Condition &cond) {
        index_conds_[predicates_[column]].second.op = cond.op;
        index_conds_[predicates_[column]].second.rhs_val = std::move(cond.rhs_val);
    }

    CondOp getLeft(const std::string &column) {
        return index_conds_[predicates_[column]].first;
    }

    CondOp getRight(const std::string &column) {
        return index_conds_[predicates_[column]].second;
    }

    bool cmpIndexConds(const RmRecord &rec) {
        return cmpIndexLeftConds(rec) && cmpIndexRightConds(rec);
    }

    bool cmpIndexLeftConds(const RmRecord &rec) {
        // 如果没有谓词条件直接返回真
        if (left_last_idx == 0) {
            return true;
        }
        // TODO 优化 从第一个范围查询 +1 开始匹配，因为这里只会被 index scan 算子使用
        for (int i = left_last_idx + 1; i < index_conds_.size(); ++i) {
            if (index_conds_[i].first.op != OP_INVALID && !cmpIndexCond(rec, index_conds_[i].first)) {
                return false;
            }
        }
        return true;
    }

    bool cmpIndexRightConds(const RmRecord &rec) {
        // 如果没有谓词条件直接返回真
        if (right_last_idx == 0) {
            return true;
        }
        // TODO 优化 从第一个范围查询 +1 开始匹配，因为这里只会被 index scan 算子使用
        for (int i = right_last_idx + 1; i < index_conds_.size(); ++i) {
            if (index_conds_[i].second.op != OP_INVALID && !cmpIndexCond(rec, index_conds_[i].second)) {
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

    // 左边谓词第一个不为等号的
    std::tuple<CompOp, int> getLeftLastTuple(char *&key) {
        left_last_idx = 0; // 第一个范围查询位置

        CompOp op;
        for (auto &[cond, _]: index_conds_) {
            std::ignore = _;
            op = cond.op;
            if (op == OP_INVALID) {
                break;
            }
            memcpy(key + cond.offset, cond.rhs_val.raw->data, cond.rhs_val.raw->size);
            if (op != OP_EQ) {
                break;
            }
            ++left_last_idx;
        }

        return {op, left_last_idx};
    }

    std::tuple<CompOp, int> getRightLastTuple(char *&key) {
        right_last_idx = 0; // 第一个范围查询位置

        CompOp op;
        for (auto &[_, cond]: index_conds_) {
            std::ignore = _;
            op = cond.op;
            if (op == OP_INVALID) {
                break;
            }
            memcpy(key + cond.offset, cond.rhs_val.raw->data, cond.rhs_val.raw->size);
            if (op != OP_EQ) {
                break;
            }
            ++right_last_idx;
        }

        return {op, right_last_idx};
    }

    std::vector<std::pair<CondOp, CondOp> > &getIndexConds() {
        return index_conds_;
    }

private:
    int left_last_idx = 0;
    int right_last_idx = 0;
    std::unordered_map<std::string, int> predicates_;
    std::vector<std::pair<CondOp, CondOp> > index_conds_;
};
