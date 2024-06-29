#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

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

struct AggregateKey {
    // 用 rmcord 可能更好
    std::vector<Value> group_bys;
};

struct AggregateValue {
    // 用 rmcord 可能更好，select 里面的聚合值
    std::vector<Value> values;
    // having 里面的聚合值
    std::vector<Value> having_values;
};

struct AggregateKeyHash {
    std::size_t operator()(const AggregateKey &key) const {
        std::size_t hash = 0;
        for (const auto &value: key.group_bys) {
            switch (value.type) {
                case TYPE_INT: {
                    hash ^= std::hash<int>()(value.int_val);
                    break;
                }
                case TYPE_FLOAT: {
                    hash ^= std::hash<float>()(value.float_val);
                    break;
                }
                case TYPE_STRING: {
                    hash ^= std::hash<std::string>()(value.str_val);
                    break;
                }
            }
            // hash ^= std::hash<int>()(value.int_val) ^ std::hash<float>()(value.float_val) ^ std::hash<std::string>()(
            //     value.str_val);
        }
        return hash;
    }
};

struct AggregateKeyEqual {
    bool operator()(const AggregateKey &lhs, const AggregateKey &rhs) const {
        return lhs.group_bys == rhs.group_bys;
    }
};

class AggregateHashTable {
public:
    AggregateHashTable(const std::vector<AggType> &agg_types,
                       const std::vector<Condition> &having_conds) : agg_types_(agg_types),
                                                                     having_conds_(having_conds) {
    }

    auto generateInitialAggregateValue(const AggregateValue &input) -> AggregateValue {
        std::vector<Value> values;
        std::vector<Value> having_values;
        for (std::size_t i = 0; i < agg_types_.size(); ++i) {
            switch (agg_types_[i]) {
                case AGG_COUNT: {
                    Value v;
                    v.set_int(1);
                    v.init_raw(sizeof(int));
                    values.emplace_back(v);
                    break;
                }
                case AGG_MAX:
                case AGG_MIN:
                case AGG_SUM:
                    // 直接初始化为输入值
                    values.emplace_back(input.values[i]);
                    break;
                case AGG_COL:
                    // 占位
                    values.emplace_back();
                    break;
                default:
                    throw InternalError("Unexpected aggregate type！");
            }
        }

        for (std::size_t i = 0; i < having_conds_.size(); ++i) {
            switch (having_conds_[i].agg_type) {
                case AGG_COUNT: {
                    Value v;
                    v.set_int(1);
                    v.init_raw(sizeof(int));
                    having_values.emplace_back(v);
                    break;
                }
                case AGG_MAX:
                case AGG_MIN:
                case AGG_SUM:
                    // 直接初始化为输入值
                    having_values.emplace_back(input.having_values[i]);
                    break;
                case AGG_COL:
                default:
                    throw InternalError("Unexpected aggregate type！");
            }
        }

        return {values, having_values};
    }

    void combineAggregateValues(AggregateValue *result, const AggregateValue &input) {
        for (std::size_t i = 0; i < agg_types_.size(); i++) {
            switch (agg_types_[i]) {
                case AGG_COUNT:
                    // result->values[i].set_int(result->values[i].int_val + 1);
                    result->values[i].int_val += 1;
                    break;
                case AGG_MAX: {
                    auto &lhs = result->values[i];
                    auto &rhs = input.values[i];
                    if (compare(lhs.raw->data, rhs.raw->data, lhs.str_val.size(),
                                lhs.type) < 0) {
                        lhs = rhs;
                    }
                    break;
                }
                case AGG_MIN: {
                    auto &lhs = result->values[i];
                    const auto &rhs = input.values[i];
                    if (compare(lhs.raw->data, rhs.raw->data, lhs.str_val.size(),
                                lhs.type) > 0) {
                        lhs = rhs;
                    }
                    break;
                }
                case AGG_SUM: {
                    auto &lhs = result->values[i];
                    const auto &rhs = input.values[i];
                    add(lhs.raw->data, rhs.raw->data, lhs.type);
                    break;
                }
                case AGG_COL:
                    break;
            }
        }

        for (std::size_t i = 0; i < having_conds_.size(); i++) {
            switch (having_conds_[i].agg_type) {
                case AGG_COUNT:
                    // result->values[i].set_int(result->values[i].int_val + 1);
                    result->having_values[i].int_val += 1;
                    break;
                case AGG_MAX: {
                    auto &lhs = result->having_values[i];
                    auto &rhs = input.having_values[i];
                    if (compare(lhs.raw->data, rhs.raw->data, lhs.str_val.size(),
                                lhs.type) < 0) {
                        lhs = rhs;
                    }
                    break;
                }
                case AGG_MIN: {
                    auto &lhs = result->having_values[i];
                    const auto &rhs = input.having_values[i];
                    if (compare(lhs.raw->data, rhs.raw->data, lhs.str_val.size(),
                                lhs.type) > 0) {
                        lhs = rhs;
                    }
                    break;
                }
                case AGG_SUM: {
                    auto &lhs = result->having_values[i];
                    const auto &rhs = input.having_values[i];
                    add(lhs.raw->data, rhs.raw->data, lhs.type);
                    break;
                }
                case AGG_COL:
                default:
                    throw InternalError("Unexpected aggregate type！");
            }
        }
    }

    void insertCombine(const AggregateKey &agg_key, const AggregateValue &agg_val) {
        if (hash_table_.count(agg_key) == 0) {
            hash_table_.emplace(agg_key, generateInitialAggregateValue(agg_val));
        } else {
            combineAggregateValues(&hash_table_[agg_key], agg_val);
        }
    }

public:
    std::unordered_map<AggregateKey, AggregateValue, AggregateKeyHash, AggregateKeyEqual> hash_table_;
    const std::vector<AggType> &agg_types_;
    const std::vector<Condition> &having_conds_;
};

class AggregateExecutor : public AbstractExecutor {
private:
    std::string tab_name_; // 表的名称
    std::vector<Condition> conds_; // scan的条件
    RmFileHandle *fh_; // 表的数据文件句柄
    std::vector<ColMeta> cols_; // scan后生成的记录的字段
    size_t len_; // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_; // 同conds_，两个字段相同
    Rid rid_;
    std::unique_ptr<RecScan> scan_; // table_iterator
    SmManager *sm_manager_;
    std::unique_ptr<RmRecord> rm_record_;
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> sel_cols_;
    std::vector<ColMeta> having_cols_;
    std::vector<AggType> agg_types_;
    std::vector<Condition> having_conds_;
    std::vector<ColMeta> group_bys_;

    AggregateHashTable ht_;
    std::unordered_map<AggregateKey, AggregateValue, AggregateKeyHash, AggregateKeyEqual>::iterator it_;
    bool has_group_col_{false};
    bool is_empty_table_{false};

public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols,
                      std::vector<AggType> agg_types, const std::vector<TabCol> &group_bys,
                      std::vector<Condition> having_conds,
                      Context *context)
        : prev_(std::move(prev)), agg_types_(std::move(agg_types)),
          having_conds_(std::move(having_conds)), ht_(agg_types_, having_conds_) {
        // sm_manager_ = sm_manager;
        // tab_name_ = std::move(tab_name);
        // conds_ = std::move(conds);
        // TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        // fh_ = sm_manager_->fhs_.at(tab_name_).get();
        // seq 的所有列
        cols_ = prev_->cols();

        // 记得先清 0
        len_ = 0;
        for (std::size_t i = 0; i < sel_cols.size(); ++i) {
            auto &sel_col = sel_cols[i];
            has_group_col_ |= agg_types_[i] == AGG_COL;
            // count(*)
            if (agg_types_[i] == AGG_COUNT && sel_col.tab_name.empty() && sel_col.col_name.empty()) {
                // 占位
                sel_cols_.emplace_back();
                sel_cols_.back().type = TYPE_INT;
                sel_cols_.back().len = sizeof(int);
                // 改和不改都没事
                sel_cols_.back().offset = sizeof(int);
            } else {
                sel_cols_.emplace_back(*get_col(cols_, sel_col));
                // count 输出整数，需要改变列类型和偏移量
                if (agg_types_[i] == AGG_COUNT && sel_cols_.back().type != TYPE_INT) {
                    sel_cols_.back().type = TYPE_INT;
                    sel_cols_.back().len = sizeof(int);
                    // 改和不改都没事
                    sel_cols_.back().offset = sizeof(int);
                }
            }
            len_ += sel_cols_.back().len;
        }

        for (auto &having_cond: having_conds_) {
            // count(*)
            if (having_cond.agg_type == AGG_COUNT && having_cond.lhs_col.tab_name.empty() && having_cond.lhs_col.
                col_name.empty()) {
                // 占位
                having_cols_.emplace_back();
                having_cols_.back().type = TYPE_INT;
                having_cols_.back().len = sizeof(int);
                // 改和不改都没事
                having_cols_.back().offset = sizeof(int);
            } else {
                having_cols_.emplace_back(*get_col(cols_, having_cond.lhs_col));
                // count(course)
                if (having_cond.agg_type == AGG_COUNT && having_cols_.back().type != TYPE_INT) {
                    having_cols_.back().type = TYPE_INT;
                    having_cols_.back().len = sizeof(int);
                    // 改和不改都没事
                    having_cols_.back().offset = sizeof(int);
                }
            }
        }

        // for (auto &sel_col: sel_cols) {
        //     // TODO 注意 count(*)
        //     sel_cols_.emplace_back(*get_col(cols_, sel_col));
        //     len_ += sel_cols_.back().len;
        // }

        for (auto &group_by: group_bys) {
            group_bys_.emplace_back(*get_col(cols_, group_by));
        }

        // len_ = cols_.back().offset + cols_.back().len;
        context_ = context;
        // fed_conds_ = conds_;
    }

    void beginTuple() override {
        // 子查询要清空，也可以直接缓存？
        ht_.hash_table_.clear();
        prev_->beginTuple();

        std::vector<Value> keys(group_bys_.size());
        std::vector<Value> values(agg_types_.size());
        std::vector<Value> having_values(having_conds_.size());

        while (!prev_->is_end()) {
            keys.clear();
            values.clear();
            having_values.clear();

            rm_record_ = prev_->Next();
            for (auto &group_by: group_bys_) {
                keys.emplace_back();
                if (group_by.type == TYPE_INT) {
                    const int a = *reinterpret_cast<const int *>(rm_record_->data + group_by.offset);
                    keys.back().set_int(a);
                } else if (group_by.type == TYPE_FLOAT) {
                    const float a = *reinterpret_cast<const float *>(rm_record_->data + group_by.offset);
                    keys.back().set_float(a);
                } else if (group_by.type == TYPE_STRING) {
                    std::string s(rm_record_->data + group_by.offset, group_by.len);
                    keys.back().set_str(s);
                } else {
                    throw InternalError("Unexpected data type！");
                }
                // memcpy(keys.back().raw->data, rm_record_->data + group_by.offset, group_by.len);
                keys.back().init_raw(group_by.len);
            }

            // 获取 select 里面的聚合值
            for (std::size_t i = 0; i < agg_types_.size(); ++i) {
                switch (agg_types_[i]) {
                    case AGG_COUNT: {
                        Value v;
                        v.set_int(1);
                        v.init_raw(sizeof(int));
                        values.emplace_back(v);
                        break;
                    }
                    case AGG_MAX:
                    case AGG_MIN:
                    case AGG_SUM: {
                        // TODO 记得初始化
                        Value v;
                        if (sel_cols_[i].type == TYPE_INT) {
                            const int a = *reinterpret_cast<const int *>(rm_record_->data + sel_cols_[i].offset);
                            v.set_int(a);
                        } else if (sel_cols_[i].type == TYPE_FLOAT) {
                            const float a = *reinterpret_cast<const float *>(rm_record_->data + sel_cols_[i].offset);
                            v.set_float(a);
                        } else if (sel_cols_[i].type == TYPE_STRING) {
                            throw InternalError("You cant aggreagte string with max/min/sum");
                            std::string s(rm_record_->data + sel_cols_[i].offset, sel_cols_[i].len);
                            v.set_str(s);
                        }
                        v.init_raw(sel_cols_[i].len);
                        values.emplace_back(v);
                        break;
                    }
                    case AGG_COL:
                        values.emplace_back();
                        break;
                    default:
                        throw InternalError("Unexpected aggregate type！");
                }
            }

            // 获取 having 里面的聚合值
            for (std::size_t i = 0; i < having_conds_.size(); ++i) {
                switch (having_conds_[i].agg_type) {
                    case AGG_COUNT: {
                        Value v;
                        v.set_int(1);
                        v.init_raw(sizeof(int));
                        having_values.emplace_back(v);
                        break;
                    }
                    case AGG_MAX:
                    case AGG_MIN:
                    case AGG_SUM: {
                        // TODO 记得初始化
                        Value v;
                        if (having_cols_[i].type == TYPE_INT) {
                            const int a = *reinterpret_cast<const int *>(rm_record_->data + having_cols_[i].offset);
                            v.set_int(a);
                        } else if (having_cols_[i].type == TYPE_FLOAT) {
                            const float a = *reinterpret_cast<const float *>(rm_record_->data + having_cols_[i].offset);
                            v.set_float(a);
                        } else if (having_cols_[i].type == TYPE_STRING) {
                            std::string s(rm_record_->data + having_cols_[i].offset, having_cols_[i].len);
                            v.set_str(s);
                        }
                        v.init_raw(having_cols_[i].len);
                        having_values.emplace_back(v);
                        break;
                    }
                    case AGG_COL:
                    default:
                        throw InternalError("Unexpected aggregate type！");
                }
            }

            ht_.insertCombine({keys}, {values, having_values});
            prev_->nextTuple();
        }

        it_ = ht_.hash_table_.begin();
        // 空表
        if (it_ == ht_.hash_table_.end()) {
            // 空表且有group by，但是没有key直接输出空表
            if (!group_bys_.empty() && has_group_col_) {
                return;
            }
            is_empty_table_ = true;
        }

        while (it_ != ht_.hash_table_.end()) {
            std::size_t i = 0;
            for (; i < it_->second.having_values.size(); ++i) {
                if (!cmp_cond(it_->second.having_values[i], having_conds_[i].rhs_val, having_conds_[i])) {
                    break;
                }
            }
            if (i == having_conds_.size()) {
                break;
            }
            ++it_;
        }
    }

    void nextTuple() override {
        if (is_empty_table_) {
            is_empty_table_ = false;
            return;
        }
        ++it_;
        while (it_ != ht_.hash_table_.end()) {
            std::size_t i = 0;
            for (; i < it_->second.having_values.size(); ++i) {
                if (!cmp_cond(it_->second.having_values[i], having_conds_[i].rhs_val, having_conds_[i])) {
                    break;
                }
            }
            if (i == having_conds_.size()) {
                break;
            }
            ++it_;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        auto record = std::make_unique<RmRecord>(len_);
        // count 输出 0，其他输出空
        if (is_empty_table_) {
            int offset = 0;
            for (std::size_t i = 0; i < agg_types_.size(); ++i) {
                switch (agg_types_[i]) {
                    case AGG_COUNT: {
                        int zero = 0;
                        memcpy(record->data + offset, &zero, sizeof(int));
                        offset += sizeof(int);
                        break;
                    }
                    case AGG_MAX:
                    case AGG_MIN:
                    case AGG_SUM:
                    // {
                    //     // 为了输出空改为字符串类型，原来 len 不变
                    //     sel_cols_[i].type = TYPE_STRING;
                    //     std::string s;
                    //     memcpy(record->data + offset, s.c_str(), sel_cols_[i].len);
                    //     offset += sel_cols_[i].len;
                    //     break;
                    // }
                    case AGG_COL:
                    default:
                        throw InternalError("Unsupported aggregate null type！");
                }
            }
            return std::move(record);
        }

        int offset = 0;

        // for (std::size_t i = 0; i < sel_cols_.size(); ++i) {
        //     auto &sel_col = sel_cols_[i];
        //     if (agg_types_[i] == AGG_COL) {
        //         auto &&pos = std::find_if(group_bys_.begin(), group_bys_.end(), [&](ColMeta& col_meta) {
        //             return col_meta.tab_name == sel_col.tab_name && col_meta.name == sel_col.name;
        //         });
        //         if (pos == group_bys_.end()) {
        //             throw InternalError("SELECT 列表中不能出现没有在 GROUP BY 子句中的非聚集列！");
        //         }
        //         memcpy(record->data + offset, ->data, group_bys_[i].len);
        //     }
        //
        //     memcpy(record->data + offset, key.raw->data, group_bys_[i].len);
        //     offset += group_bys_[i].len;
        // }

        // 先生成左 key 右 value 的形式，具体列顺序由投影算子执行
        if (has_group_col_) {
            for (std::size_t i = 0; i < group_bys_.size(); ++i) {
                auto &key = it_->first.group_bys[i];
                memcpy(record->data + offset, key.raw->data, group_bys_[i].len);
                offset += group_bys_[i].len;
            }
        }

        for (std::size_t i = 0; i < agg_types_.size(); ++i) {
            if (agg_types_[i] == AGG_COL) {
                continue;
            }
            auto &value = it_->second.values[i];
            if (agg_types_[i] == AGG_COUNT) {
                memcpy(record->data + offset, &value.int_val, sel_cols_[i].len);
            } else {
                memcpy(record->data + offset, value.raw->data, sel_cols_[i].len);
            }
            offset += sel_cols_[i].len;
        }

        // assert(offset == len_);
        return std::move(record);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const {
        // 空表输出一次
        if (is_empty_table_) {
            return false;
        }
        return it_ == ht_.hash_table_.end();
    }

    const std::vector<ColMeta> &cols() const override { return sel_cols_; }

    size_t tupleLen() const override { return len_; }

    // 判断是否满足单个谓词条件
    static bool cmp_cond(const Value &l_rec, const Value &r_rec, const Condition &cond) {
        if (l_rec.type != r_rec.type) {
            throw IncompatibleTypeError(coltype2str(l_rec.type), coltype2str(r_rec.type));
        }
        if (l_rec.type == TYPE_INT) {
            memcpy(l_rec.raw->data, &l_rec.int_val, sizeof(int));
        }

        auto &&lhs_value = l_rec.raw->data;
        auto &&rhs_value = r_rec.raw->data;

        int cmp = compare(lhs_value, rhs_value, l_rec.str_val.size(), l_rec.type);
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

    bool cmp_conds(const Value &l_rec, const Value &r_rec, const std::vector<Condition> &conds) {
        return std::all_of(conds.begin(), conds.end(), [&](const Condition &cond) {
            return cmp_cond(l_rec, r_rec, cond);
        });
    }

    std::string getType() { return "AggregateExecutor"; }
};
