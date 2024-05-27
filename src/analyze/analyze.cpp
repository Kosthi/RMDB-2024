/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */
std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse) {
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse)) {
        // 处理表名
        query->tables = std::move(x->tabs);
        /** TODO: 检查表是否存在 */
        for (auto &table: query->tables) {
            if (!sm_manager_->db_.is_table(table)) {
                throw TableNotFoundError(table);
            }
        }

        // 处理target list，再target list中添加上表名，例如 a.id
        for (auto &item: x->select_list) {
            query->cols.emplace_back(TabCol{item->col->tab_name, item->col->col_name});
            query->agg_types.emplace_back(item->type);
            if (query->agg_types.back() != AGG_COL && item->alias.empty()) {
                switch (query->agg_types.back()) {
                    case AGG_COUNT: {
                        // count(*)
                        if (item->col->col_name.empty()) {
                            query->alias.emplace_back("COUNT(*)");
                        } else {
                            query->alias.emplace_back("COUNT(" + item->col->col_name + ")");
                        }
                        break;
                    }
                    case AGG_MAX: {
                        query->alias.emplace_back("MAX(" + item->col->col_name + ")");
                        break;
                    }
                    case AGG_MIN: {
                        query->alias.emplace_back("MIN(" + item->col->col_name + ")");
                        break;
                    }
                    case AGG_SUM: {
                        query->alias.emplace_back("SUM(" + item->col->col_name + ")");
                        break;
                    }
                    default:
                        throw InternalError("Unexpected aggregate type！");
                }
            } else {
                query->alias.emplace_back(item->alias);
            }
        }

        std::vector<ColMeta> all_cols;
        // 得到扫描的表的所有列
        get_all_cols(query->tables, all_cols);

        if (query->cols.empty()) {
            // select all columns
            if (!x->group_bys.empty() || !x->havings.empty()) {
                throw InternalError("select * 不能包含聚合和分组子句！");
            }
            for (auto &col: all_cols) {
                query->cols.emplace_back(TabCol{col.tab_name, col.name});
            }
        } else {
            // 表名不存在，从列名推断表名；如果表名存在则对列做检查
            for (std::size_t i = 0; i < query->cols.size(); ++i) {
                // COUNT(*)
                if (query->agg_types[i] == AGG_COUNT && query->cols[i].tab_name.empty() && query->cols[i].col_name.
                    empty()) {
                    continue;
                }
                // TODO 直接引用减少拷贝
                query->cols[i] = check_column(all_cols, query->cols[i]); // 列元数据校验
            }
        }

        // 处理 where 条件
        get_clause(x->conds, query->conds);

        // 处理 group by 条件
        for (auto &group_by: x->group_bys) {
            query->group_bys.emplace_back(TabCol{group_by->tab_name, group_by->col_name});
        }

        // 填充表名和列校验
        for (auto &tab_col: query->group_bys) {
            tab_col = check_column(all_cols, tab_col);
        }

        // 没有 group，不能出现 AGG_COL，必须都是聚合函数
        if (query->group_bys.empty()) {
            if (!x->havings.empty()) {
                throw InternalError("没有 GROUP BY 子句但是有 HAVING 子句！");
            }
            bool has_col = false;
            bool has_agg = false;
            for (auto &agg_type: query->agg_types) {
                has_col |= agg_type == AGG_COL;
                has_agg |= agg_type != AGG_COL;
                if (has_col && has_agg) {
                    throw InternalError("没有 GROUP BY 子句且有聚合函数，但包含非聚合值！");
                }
            }
        } else {
            // SELECT 列表中不能出现没有在 GROUP BY 子句中的非聚集列
            // select id , score from grade group by course;
            for (std::size_t i = 0; i < query->cols.size(); ++i) {
                if (query->agg_types[i] == AGG_COL) {
                    auto &&pos = std::find_if(query->group_bys.begin(), query->group_bys.end(), [&](TabCol &tab_col) {
                        return tab_col.tab_name == query->cols[i].tab_name && tab_col.col_name == query->cols[i].
                               col_name;
                    });
                    if (pos == query->group_bys.end()) {
                        throw InternalError("SELECT 列表中不能出现没有在 GROUP BY 子句中的非聚集列！");
                    }
                }
            }
        }

        // 处理 having 条件
        get_having_clause(x->havings, query->havings);

        // 推断表名和检查左右类型是否匹配
        check_clause(query->tables, query->conds);
        check_clause(query->tables, query->havings);
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        /** TODO: */
        // 构造set_clauses
        for (auto &set: x->set_clauses) {
            // set语句只对某个表修改，不需要表名
            query->set_clauses.emplace_back(SetClause{TabCol{"", set->col_name}, convert_sv_value(set->val)});
        }

        // 检查set左右值类型是否相同
        auto &tab_meta = sm_manager_->db_.get_table(x->tab_name);
        for (auto &set: query->set_clauses) {
            // 从表元信息中获取列元信息
            auto &&col_meta = tab_meta.get_col(set.lhs.col_name);
            int len = col_meta->len;
            // 兼容 int -> float
            if (col_meta->type == TYPE_FLOAT && set.rhs.type == TYPE_INT) {
                len = sizeof(float);
                set.rhs.set_float(static_cast<float>(set.rhs.int_val));
            } else if (col_meta->type != set.rhs.type) {
                throw IncompatibleTypeError(coltype2str(col_meta->type), coltype2str(set.rhs.type));
            }
            set.rhs.init_raw(len);
        }

        // 处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        // 处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        // 处理insert 的values值
        for (auto &sv_val: x->vals) {
            query->values.emplace_back(convert_sv_value(sv_val));
        }
    } else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}

TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        for (auto &col: all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = std::move(tab_name);
    } else {
        /** TODO: Make sure target column exists */
        bool not_exist = true;
        for (auto &col: all_cols) {
            // select t.id from t,d where id = 1;
            if (col.tab_name == target.tab_name && col.name == target.col_name) {
                not_exist = false;
                break;
            }
        }
        if (not_exist) {
            throw ColumnNotFoundError(target.col_name);
        }
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name: tab_names) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr> > &sv_conds,
                         std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr: sv_conds) {
        Condition cond;
        cond.agg_type = AGG_COL;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.emplace_back(cond);
    }
}

void Analyze::get_having_clause(const std::vector<std::shared_ptr<ast::HavingExpr> > &sv_conds,
                                std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr: sv_conds) {
        Condition cond;
        // having 语句左侧必须是聚合函数
        if (expr->lhs->type == AGG_COL) {
            throw InternalError("Having 语句左侧必须是聚合函数！");
        }
        cond.agg_type = expr->lhs->type;
        // 如果是 having count(*) > 1 这里为空
        cond.lhs_col = {.tab_name = expr->lhs->col->tab_name, .col_name = expr->lhs->col->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            // 右边一定是数值
            throw InternalError("Having 语句右侧应该为数值！");
            // cond.is_rhs_val = false;
            // cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.emplace_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    // auto all_cols = get_all_cols(tab_names);
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    // Get raw values in where clause
    for (auto &cond: conds) {
        // count(*)
        if (cond.agg_type == AGG_COUNT && cond.lhs_col.tab_name.empty() && cond.lhs_col.col_name.empty()) {
            if (cond.rhs_val.type == TYPE_INT) {
                cond.rhs_val.init_raw(sizeof(int));
            } else {
                throw IncompatibleTypeError("INT", coltype2str(cond.rhs_val.type));
            }
            continue;
        }
        // Infer table name from column name
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        }
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        // 这里假设 where 语句左值必须为列值
        ColType lhs_type = cond.agg_type == AGG_COUNT ? TYPE_INT : lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            // having count(course) > 1
            // having count(course) > 1.0
            // having count(course) > '1.0'
            // 左 char 右 int，左边在聚合算子内处理
            if (cond.agg_type == AGG_COUNT) {
                if (cond.rhs_val.type == TYPE_INT) {
                    cond.rhs_val.init_raw(sizeof(int));
                } else if (cond.rhs_val.type == TYPE_FLOAT) {
                    cond.rhs_val.init_raw(sizeof(float));
                } else if (cond.rhs_val.type == TYPE_STRING) {
                    cond.rhs_val.init_raw(lhs_col->len);
                } else {
                    throw InternalError("Unexpected data type！");
                }
            } else if (lhs_type == TYPE_FLOAT && cond.rhs_val.type == TYPE_INT) {
                cond.rhs_val.set_float(static_cast<float>(cond.rhs_val.int_val));
                cond.rhs_val.init_raw(sizeof(float));
            } else {
                // 左边的类型可能和右边不同不能直接 col_len
                // 比如 where course > 0 having min(course) > 0.0
                if (cond.rhs_val.type == TYPE_INT) {
                    cond.rhs_val.init_raw(sizeof(int));
                } else if (cond.rhs_val.type == TYPE_FLOAT) {
                    cond.rhs_val.init_raw(sizeof(float));
                } else if (cond.rhs_val.type == TYPE_STRING) {
                    cond.rhs_val.init_raw(lhs_col->len);
                }
            }
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
    }
}

Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}
