#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"

class SortMergeJoinExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> left_; // 左儿子节点（需要join的表，已经sort）
    std::unique_ptr<AbstractExecutor> right_; // 右儿子节点（需要join的表，已经sort）
    size_t len_; // join后获得的每条记录的长度
    std::vector<ColMeta> cols_; // join后获得的记录的字段
    std::vector<Condition> fed_conds_; // join条件
    std::unique_ptr<RmRecord> rm_record_;
    std::unique_ptr<RmRecord> lhs_rec_;
    std::shared_ptr<RmRecord> rhs_rec_; // 共享指针，避免拷贝开销
    bool is_right_empty_;
    Condition last_cond_;
    int last_cmp_;
    bool early_end_; // 当发现剩下元组不存在连接的情况后提前退出
    bool is_rollback_; // 是否有回退的情况
    size_t match_cnts_;
    size_t rollback_cnts_; // 回滚后需要返回的元组数量
    std::shared_ptr<RmRecord> prev_rhs_rec_;

    // for index scan
    void write_sorted_results() {
        // 以期望格式写入 sorted_results.txt
        auto out_expected_file = std::fstream("sorted_results.txt", std::ios::out | std::ios::app);

        // 打印右表头
        out_expected_file << "|";
        for (auto &col_meta: right_->cols()) {
            out_expected_file << " " << col_meta.name << " |";
        }
        out_expected_file << "\n";

        // 右表先开始
        for (right_->beginTuple(); !right_->is_end(); right_->nextTuple()) {
            rhs_rec_ = right_->Next();
            // 打印记录
            std::vector<std::string> columns;
            columns.reserve(right_->cols().size());

            for (auto &col: right_->cols()) {
                std::string col_str;
                char *rec_buf = rhs_rec_->data + col.offset;
                if (col.type == TYPE_INT) {
                    col_str = std::to_string(*(int *) rec_buf);
                } else if (col.type == TYPE_FLOAT) {
                    col_str = std::to_string(*(float *) rec_buf);
                } else if (col.type == TYPE_STRING) {
                    col_str = std::string((char *) rec_buf, col.len);
                    col_str.resize(strlen(col_str.c_str()));
                }
                columns.emplace_back(col_str);
            }

            // 打印记录
            out_expected_file << "|";
            for (auto &col: columns) {
                out_expected_file << " " << col << " |";
            }
            out_expected_file << "\n";
        }

        // 打印左表头
        out_expected_file << "|";
        for (auto &col_meta: left_->cols()) {
            out_expected_file << " " << col_meta.name << " |";
        }
        out_expected_file << "\n";

        // 再左表
        for (left_->beginTuple(); !left_->is_end(); left_->nextTuple()) {
            lhs_rec_ = left_->Next();
            // 打印记录
            std::vector<std::string> columns;
            columns.reserve(left_->cols().size());

            for (auto &col: left_->cols()) {
                std::string col_str;
                char *rec_buf = lhs_rec_->data + col.offset;
                if (col.type == TYPE_INT) {
                    col_str = std::to_string(*(int *) rec_buf);
                } else if (col.type == TYPE_FLOAT) {
                    col_str = std::to_string(*(float *) rec_buf);
                } else if (col.type == TYPE_STRING) {
                    col_str = std::string((char *) rec_buf, col.len);
                    col_str.resize(strlen(col_str.c_str()));
                }
                columns.emplace_back(col_str);
            }

            // 打印记录
            out_expected_file << "|";
            for (auto &col: columns) {
                out_expected_file << " " << col << " |";
            }
            out_expected_file << "\n";
        }

        out_expected_file.close();
    }

public:
    SortMergeJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                          std::vector<Condition> conds): left_(std::move(left)), right_(std::move(right)),
                                                         fed_conds_(std::move(conds)) {
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col: right_cols) {
            col.offset += left_->tupleLen();
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        last_cond_ = fed_conds_[0];
    }

    void beginTuple() override {
        is_right_empty_ = false;
        early_end_ = false;
        match_cnts_ = 0;
        is_rollback_ = false;
        rollback_cnts_ = 0;

        // 为了 sort_results.txt 文件内容与测试一致，右算子先开始
        right_->beginTuple();
        // 右表为空时直接返回
        if (right_->is_end()) {
            is_right_empty_ = true;
            return;
        }
        left_->beginTuple();
        // 左表为空时直接返回
        if (left_->is_end()) {
            return;
        }

        do {
            lhs_rec_ = left_->Next();
            rhs_rec_ = right_->Next();
            // 找到第一个匹配的元组，对应非!=运算符，不存在则直接结束算子
            // match_cnts_ <= 1
            while (!right_->is_end()) {
                // 如果找到匹配的直接 return
                if (cmp_conds(lhs_rec_.get(), rhs_rec_.get(), fed_conds_, cols_)) {
                    rm_record_ = std::make_unique<RmRecord>(len_);
                    // 拷贝左右元组
                    memcpy(rm_record_->data, lhs_rec_->data, left_->tupleLen());
                    memcpy(rm_record_->data + left_->tupleLen(), rhs_rec_->data, right_->tupleLen());
                    // 第一次匹配，拷贝一份
                    if (++match_cnts_ == 1) {
                        prev_rhs_rec_ = rhs_rec_;
                    }
                    return;
                }
                // 不匹配，看最后一次比较，有三种情况
                // 1. 左表+1
                // 2. 右表+1
                // 3. 左右表都+1
                switch (last_cond_.op) {
                    case OP_EQ: {
                        // 1. 1  2 左表+1
                        // 2. 2  1 右表+1
                        if (last_cmp_ < 0) {
                            left_->nextTuple();
                            if (left_->is_end()) {
                                return;
                            }
                            lhs_rec_ = left_->Next();
                        } else {
                            right_->nextTuple();
                            // 如果右表结束了，说明没有元组等于左表当前元组
                            // 左表当前及后面的元组严格大于右表所有元组，直接退出
                            if (right_->is_end()) {
                                early_end_ = true;
                                return;
                            }
                            rhs_rec_ = right_->Next();
                        }
                        break;
                    }
                    case OP_NE: {
                        // 1. 1  1 // 右表+1
                        right_->nextTuple();
                        // 如果右表结束了，说明没有元组不等于左表，左表应该+1，右表从头开始扫描
                        if (right_->is_end()) {
                            break;
                        }
                        rhs_rec_ = right_->Next();
                        break;
                    }
                    case OP_LT: {
                        // 1. 1  1 右表+1
                        // 2. 2  1 右表+1
                        right_->nextTuple();
                        // 如果右表结束了，说明没有元组小于左表当前元组，左表当前及后面的元组严格大于等于右表所有元组，直接退出
                        if (right_->is_end()) {
                            early_end_ = true;
                            return;
                        }
                        rhs_rec_ = right_->Next();
                        break;
                    }
                    case OP_GT: {
                        // 1. 1  1 左表+1
                        // 2. 1  2 左表+1
                        left_->nextTuple();
                        if (left_->is_end()) {
                            return;
                        }
                        lhs_rec_ = left_->Next();
                        break;
                    }
                    case OP_LE: {
                        // 1. 2  1 右表+1
                        right_->nextTuple();
                        // 如果右表结束了，说明没有元组小于等于左表当前元组
                        // 即左表当前及后面的元组严格大于右表所有元组，直接退出
                        if (right_->is_end()) {
                            early_end_ = true;
                            return;
                        }
                        rhs_rec_ = right_->Next();
                        break;
                    }
                    case OP_GE: {
                        // 1. 1  2 左表+1
                        left_->nextTuple();
                        if (left_->is_end()) {
                            return;
                        }
                        lhs_rec_ = left_->Next();
                        break;
                    }
                    default:
                        throw InternalError("Unexpected op type！");
                }
            }
            // 只有结果不匹配且谓词为不等于才会到这里
            // !=
            // std::cerr << last_cond_.op << std::endl;
            assert(last_cond_.op == OP_NE);
            left_->nextTuple();
            right_->beginTuple();
        } while (!left_->is_end());
    }

    void nextTuple() override {
        if (left_->is_end()) {
            return;
        }

        // 回滚状态右指针不增加，直接构造元组输出
        if (is_rollback_ && rollback_cnts_ > 0) {
            --rollback_cnts_;
            // is_rollback_ = --rollback_cnts_ > 0;
            rm_record_ = std::make_unique<RmRecord>(len_);
            // 拷贝左右元组
            memcpy(rm_record_->data, lhs_rec_->data, left_->tupleLen());
            memcpy(rm_record_->data + left_->tupleLen(), prev_rhs_rec_->data, right_->tupleLen());
            return;
        }

        if (is_rollback_) {
            is_rollback_ = false;
        } else {
            right_->nextTuple();
            if (!right_->is_end()) {
                rhs_rec_ = right_->Next();
            }
        }

        do {
            while (!right_->is_end()) {
                // 如果找到匹配的直接 return
                if (cmp_conds(lhs_rec_.get(), rhs_rec_.get(), fed_conds_, cols_)) {
                    rm_record_ = std::make_unique<RmRecord>(len_);
                    // 拷贝左右元组
                    memcpy(rm_record_->data, lhs_rec_->data, left_->tupleLen());
                    memcpy(rm_record_->data + left_->tupleLen(), rhs_rec_->data, right_->tupleLen());
                    // 第一次匹配，拷贝一份
                    if (++match_cnts_ == 1) {
                        prev_rhs_rec_ = rhs_rec_;
                    }
                    return;
                }
                // 不匹配，看最后一次比较，有三种情况
                // 1. 左表+1
                // 2. 右表+1
                // 3. 左右表都+1
                switch (last_cond_.op) {
                    case OP_EQ: {
                        // 1. 1  2 左表+1
                        // 2. 2  1 右表+1
                        if (last_cmp_ < 0) {
                            left_->nextTuple();
                            if (left_->is_end()) {
                                return;
                            }
                            lhs_rec_ = left_->Next();
                            // 1. 如果 prev_rhs_rec 满足谓词，要回滚
                            if (match_cnts_ > 0 && cmp_conds(lhs_rec_.get(), prev_rhs_rec_.get(), fed_conds_, cols_)) {
                                rollback_cnts_ = match_cnts_;
                                is_rollback_ = true;
                                // is_rollback_ = --rollback_cnts_ > 0;
                                --rollback_cnts_;
                                rm_record_ = std::make_unique<RmRecord>(len_);
                                // 拷贝左右元组
                                memcpy(rm_record_->data, lhs_rec_->data, left_->tupleLen());
                                memcpy(rm_record_->data + left_->tupleLen(), prev_rhs_rec_->data, right_->tupleLen());
                                return;
                            }
                            match_cnts_ = 0;
                            // // 和当前匹配
                            // if (cmp_conds(lhs_rec_.get(), rhs_rec_.get(), fed_conds_, cols_)) {
                            //     rm_record_ = std::make_unique<RmRecord>(len_);
                            //     // 拷贝左右元组
                            //     memcpy(rm_record_->data, lhs_rec_->data, left_->tupleLen());
                            //     memcpy(rm_record_->data + left_->tupleLen(), rhs_rec_->data, right_->tupleLen());
                            //     return;
                            // }
                        } else {
                            match_cnts_ = 0;
                            right_->nextTuple();
                            // 如果右表结束了，说明没有元组等于左表当前元组
                            // 左表当前及后面的元组严格大于右表所有元组，直接退出
                            if (right_->is_end()) {
                                early_end_ = true;
                                return;
                            }
                            rhs_rec_ = right_->Next();
                        }
                        break;
                    }
                    case OP_NE: {
                        // 1. 1  1 // 右表+1
                        right_->nextTuple();
                        // 如果右表结束了，说明没有元组不等于左表，左表应该+1，右表从头开始扫描
                        if (right_->is_end()) {
                            break;
                        }
                        rhs_rec_ = right_->Next();
                        break;
                    }
                    case OP_LT: {
                        // 1. 1  1 右表+1
                        // 2. 2  1 右表+1
                        right_->nextTuple();
                        // 如果右表结束了，说明没有元组小于左表当前元组，左表当前及后面的元组严格大于等于右表所有元组，直接退出
                        if (right_->is_end()) {
                            early_end_ = true;
                            return;
                        }
                        rhs_rec_ = right_->Next();
                        break;
                    }
                    case OP_GT: {
                        // 1. 1  1 左表+1
                        // 2. 1  2 左表+1
                        left_->nextTuple();
                        if (left_->is_end()) {
                            return;
                        }
                        lhs_rec_ = left_->Next();
                        break;
                    }
                    case OP_LE: {
                        // 1. 2  1 右表+1
                        right_->nextTuple();
                        // 如果右表结束了，说明没有元组小于等于左表当前元组
                        // 即左表当前及后面的元组严格大于右表所有元组，直接退出
                        if (right_->is_end()) {
                            early_end_ = true;
                            return;
                        }
                        rhs_rec_ = right_->Next();
                        break;
                    }
                    case OP_GE: {
                        // 1. 1  2 左表+1
                        left_->nextTuple();
                        if (left_->is_end()) {
                            return;
                        }
                        lhs_rec_ = left_->Next();
                        break;
                    }
                    default:
                        throw InternalError("Unexpected op type！");
                }
            }
            left_->nextTuple();
            if (!left_->is_end()) {
                lhs_rec_ = left_->Next();
            } else {
                return;
            }
            // TODO 只有不等号右表才需要从头开始
            // 其他符号，如果是 =
            if (last_cond_.op == OP_NE) {
                right_->beginTuple();
                // 右表为空会在 begin 的时候就检查了
                rhs_rec_ = right_->Next();
            } else {
                // 1. 如果 prev_rhs_rec 满足谓词，要回滚
                if (match_cnts_ > 0 && cmp_conds(lhs_rec_.get(), prev_rhs_rec_.get(), fed_conds_, cols_)) {
                    rollback_cnts_ = match_cnts_;
                    is_rollback_ = true;
                    --rollback_cnts_;
                    rm_record_ = std::make_unique<RmRecord>(len_);
                    // 拷贝左右元组
                    memcpy(rm_record_->data, lhs_rec_->data, left_->tupleLen());
                    memcpy(rm_record_->data + left_->tupleLen(), prev_rhs_rec_->data, right_->tupleLen());
                    return;
                }
                // assert(false);
                // match_cnts_ = 0;
                // // 判断左表新元组是否满足谓词条件，满足继续连接，否则直接 early return
                // if (cmp_conds(lhs_rec_.get(), rhs_rec_.get(), fed_conds_, cols_)) {
                //     rm_record_ = std::make_unique<RmRecord>(len_);
                //     // 拷贝左右元组
                //     memcpy(rm_record_->data, lhs_rec_->data, left_->tupleLen());
                //     memcpy(rm_record_->data + left_->tupleLen(), rhs_rec_->data, right_->tupleLen());
                //     return;
                // }
                // 新元组不满足，后面肯定不满足，直接 early return
                early_end_ = true;
                return;
            }
        } while (!left_->is_end());
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(rm_record_);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const { return left_->is_end() || is_right_empty_ || early_end_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    size_t tupleLen() const override { return len_; }

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

    // 判断是否满足单个谓词条件
    bool cmp_cond(const RmRecord *lhs_rec, const RmRecord *rhs_rec, const Condition &cond,
                  const std::vector<ColMeta> &rec_cols) {
        last_cond_ = cond;

        // 左边必然为列值
        const auto &lhs_col_meta = get_col(rec_cols, cond.lhs_col);
        const char *lhs_data = lhs_rec->data + lhs_col_meta->offset;
        const char *rhs_data;
        ColType rhs_type;

        // 提取左值与右值的数据和类型
        // 常值
        if (cond.is_rhs_val) {
            rhs_type = cond.rhs_val.type;
            rhs_data = cond.rhs_val.raw->data;
        } else {
            // 列值
            const auto &rhs_col_meta = get_col(rec_cols, cond.rhs_col);
            rhs_type = rhs_col_meta->type;
            rhs_data = rhs_rec->data + rhs_col_meta->offset - left_->tupleLen(); // 记得减去左边的 tuplelen
        }

        if (lhs_col_meta->type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_col_meta->type), coltype2str(rhs_type));
        }

        last_cmp_ = compare(lhs_data, rhs_data, lhs_col_meta->len, lhs_col_meta->type);
        switch (cond.op) {
            case OP_EQ: return last_cmp_ == 0;
            case OP_NE: return last_cmp_ != 0;
            case OP_LT: return last_cmp_ < 0;
            case OP_GT: return last_cmp_ > 0;
            case OP_LE: return last_cmp_ <= 0;
            case OP_GE: return last_cmp_ >= 0;
            default:
                throw InternalError("Unexpected op type！");
        }
    }

    bool cmp_conds(const RmRecord *lhs_rec, const RmRecord *rhs_rec, const std::vector<Condition> &conds,
                   const std::vector<ColMeta> &rec_cols) {
        return std::all_of(conds.begin(), conds.end(), [&](const Condition &cond) {
            return cmp_cond(lhs_rec, rhs_rec, cond, rec_cols);
        });
    }
};
