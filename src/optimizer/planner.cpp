/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"

#include <memory>

#include "index/ix.h"
#include "record_printer.h"

// 目前的索引匹配规则为：完全匹配索引字段，且全部为单点查询，不会自动调整where条件的顺序
// 支持自动调整where顺序，最长前缀匹配
bool Planner::get_index_cols(std::string &tab_name, std::vector<Condition> &curr_conds,
                             std::vector<std::string> &index_col_names) {
    // index_col_names.clear();
    // for (auto &cond: curr_conds) {
    //     if (cond.is_rhs_val && cond.op == OP_EQ && cond.lhs_col.tab_name == tab_name)
    //         index_col_names.push_back(cond.lhs_col.col_name);
    // }

    if (curr_conds.empty()) {
        return false;
    }

    // TODO
    TabMeta &tab = sm_manager_->db_.get_table(tab_name);
    // TODO 优化：减少索引文件名长度，提高匹配效率
    // conds重复去重

    std::set<std::string> index_set; // 快速查找
    std::unordered_map<std::string, int> conds_map; // 列名 -> Cond
    std::unordered_map<std::string, int> repelicate_conds_map;
    for (std::size_t i = 0; i < curr_conds.size(); ++i) {
        auto &col_name = curr_conds[i].lhs_col.col_name;
        if (index_set.count(col_name) == 0) {
            index_set.emplace(col_name);
            conds_map.emplace(col_name, i);
        } else {
            repelicate_conds_map.emplace(col_name, i);
        }
    }

    int max_len = 0, max_equals = 0, cur_len = 0, cur_equals = 0;
    for (auto &[index_name, index]: tab.indexes) {
        cur_len = cur_equals = 0;
        auto &cols = index.cols;
        for (auto &[_, col]: index.cols) {
            if (index_set.count(col.name) == 0) {
                break;
            }
            if (curr_conds[conds_map[col.name]].op == OP_EQ) {
                ++cur_equals;
            }
            ++cur_len;
        }
        // 如果有 where a = 1, b = 1, c > 1;
        // index(a, b, c), index(a, b, c, d);
        // 应该匹配最合适的，避免索引查询中带来的额外拷贝开销
        if (cur_len > max_len && cur_len < curr_conds.size()) {
            // 匹配最长的
            max_len = cur_len;
            index_col_names.clear();
            for (int i = 0; i < index.cols.size(); ++i) {
                index_col_names.emplace_back(index.cols[i].second.name);
            }
        } else if (cur_len == curr_conds.size()) {
            max_len = cur_len;
            // 最长前缀相等选择等号多的
            if (index_col_names.empty()) {
                for (int i = 0; i < index.cols.size(); ++i) {
                    index_col_names.emplace_back(index.cols[i].second.name);
                }
                // for (int i = 0; i < cur_len; ++i) {
                //     index_col_names.emplace_back(index.cols[i].name);
                // }
                // = = >  等号优先   = > =     = =
                // = > =           = = > _   = = _
                // } else if(index_col_names.size() > index.cols.size()) {
                //     // 选择最合适的，正好满足，这样减少索引查找的 memcpy
                //     index_col_names.clear();
                //     for (int i = 0; i < index.cols.size(); ++i) {
                //         index_col_names.emplace_back(index.cols[i].name);
                //     }
                // = = >  等号优先   = > =     = =
                // = > =           = = > _   = = _
                // 谁等号多选谁，不管是否合适
            } else if (cur_equals > max_equals) {
                max_equals = cur_equals;
                // cur_len >= cur_equals;
                index_col_names.clear();
                for (int i = 0; i < index.cols.size(); ++i) {
                    index_col_names.emplace_back(index.cols[i].second.name);
                }
                // for (int i = 0; i < cur_len; ++i) {
                //     index_col_names.emplace_back(index.cols[i].name);
                // }
            }
        }
    }

    // 没有索引
    if (index_col_names.empty()) {
        return false;
    }

    std::vector<Condition> fed_conds; // 理想谓词

    // 连接剩下的非索引列
    // 先清除已经在set中的
    for (auto &index_name: index_col_names) {
        if (index_set.count(index_name)) {
            index_set.erase(index_name);
            fed_conds.emplace_back(std::move(curr_conds[conds_map[index_name]]));
        }
    }

    // 连接 set 中剩下的
    for (auto &index_name: index_set) {
        fed_conds.emplace_back(std::move(curr_conds[conds_map[index_name]]));
    }

    // 连接重复的，如果有
    for (auto &[index_name, idx]: repelicate_conds_map) {
        fed_conds.emplace_back(std::move(curr_conds[repelicate_conds_map[index_name]]));
    }

    curr_conds = std::move(fed_conds);

    // 检查正确与否
    for (auto &index_name: index_col_names) {
        std::cout << index_name << ",";
    }
    std::cout << "\n";

    // if (tab.is_index(index_col_names)) return true;
    return true;
}

/**
 * @brief 表算子条件谓词生成
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
std::vector<Condition>
Planner::pop_conds(std::vector<Condition> &conds, const std::string &tab_names, Context *context) {
    // auto has_tab = [&](const std::string &tab_name) {
    //     return std::find(tab_names.begin(), tab_names.end(), tab_name) != tab_names.end();
    // };
    // 根据表不同在表上生成谓词
    std::vector<Condition> solved_conds;
    for (auto &&it = conds.begin(); it != conds.end();) {
        if ((tab_names == it->lhs_col.tab_name && (it->is_rhs_val || it->is_sub_query)) || (
                it->lhs_col.tab_name == it->rhs_col.tab_name)) {
            // 如果是子查询且不为值列表，先生成子查询计划
            if (it->is_sub_query && it->sub_query != nullptr) {
                it->sub_query_plan = generate_select_plan(it->sub_query, context);
            }
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            ++it;
        }
    }
    return std::move(solved_conds);
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan) {
    if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        if (x->tab_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if (x->tab_name_.compare(cond->rhs_col.tab_name) == 0) {
            return 2;
        } else {
            return 0;
        }
    } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        int left_res = push_conds(cond, x->left_);
        // 条件已经下推到左子节点
        if (left_res == 3) {
            return 3;
        }
        int right_res = push_conds(cond, x->right_);
        // 条件已经下推到右子节点
        if (right_res == 3) {
            return 3;
        }
        // 左子节点或右子节点有一个没有匹配到条件的列
        if (left_res == 0 || right_res == 0) {
            return left_res + right_res;
        }
        // 左子节点匹配到条件的右边
        if (left_res == 2) {
            // 需要将左右两边的条件变换位置
            std::map<CompOp, CompOp> swap_op = {
                {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
            };
            std::swap(cond->lhs_col, cond->rhs_col);
            cond->op = swap_op.at(cond->op);
        }
        x->conds_.emplace_back(std::move(*cond));
        return 3;
    }
    return false;
}

std::shared_ptr<Plan> Planner::pop_scan(int *scantbl, const std::string &table, std::vector<std::string> &joined_tables,
                                        std::vector<std::shared_ptr<Plan> > plans) {
    for (size_t i = 0; i < plans.size(); i++) {
        auto x = std::dynamic_pointer_cast<ScanPlan>(plans[i]);
        if (x->tab_name_ == table) {
            scantbl[i] = 1;
            joined_tables.emplace_back(x->tab_name_);
            return plans[i];
        }
    }
    return nullptr;
}

std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context) {
    //TODO 实现逻辑优化规则

    return query;
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context) {
    std::shared_ptr<Plan> plan = make_one_rel(query, context);

    // 其他物理优化

    // 处理orderby
    plan = generate_sort_plan(query, std::move(plan));

    return plan;
}

std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query, Context *context) {
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    std::vector<std::string> tables = query->tables;
    // // Scan table , 生成表算子列表tab_nodes
    std::vector<std::shared_ptr<Plan> > table_scan_executors(tables.size());
    for (size_t i = 0; i < tables.size(); i++) {
        auto curr_conds = pop_conds(query->conds, tables[i], context);
        // int index_no = get_indexNo(tables[i], curr_conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(tables[i], curr_conds, index_col_names);
        if (index_exist == false) {
            // 该表没有索引
            index_col_names.clear();
            table_scan_executors[i] =
                    std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, tables[i], curr_conds, index_col_names);
        } else {
            // 存在索引
            // 且在排序列上，不需要排序
            if (x->has_sort) {
                for (auto &cond: curr_conds) {
                    if (cond.lhs_col == query->sort_bys || cond.rhs_col == query->sort_bys) {
                        x->has_sort = false;
                        break;
                    }
                }
            }
            table_scan_executors[i] =
                    std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, tables[i], curr_conds, index_col_names);
        }
    }
    // TODO 这里先假设子查询不需要 join
    // 只有一个表，不需要join。
    if (tables.size() == 1) {
        return table_scan_executors[0];
    }
    // 获取where条件
    auto conds = std::move(query->conds);
    std::shared_ptr<Plan> table_join_executors;

    int scantbl[tables.size()];
    for (size_t i = 0; i < tables.size(); i++) {
        scantbl[i] = -1;
    }
    // 假设在ast中已经添加了jointree，这里需要修改的逻辑是，先处理jointree，然后再考虑剩下的部分
    if (conds.size() >= 1) {
        // 有连接条件

        // 根据连接条件，生成第一层join
        std::vector<std::string> joined_tables(tables.size());
        auto it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left, right;
            std::vector<Condition> join_conds{*it};
            left = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            right = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
            // 检查左连接条件上是否有索引
            auto left_plan = std::dynamic_pointer_cast<ScanPlan>(left);
            // TODO 这里是优化成index
            if (left_plan->tag != T_IndexScan) {
                std::vector<std::string> index_col_names;
                bool index_exist = get_index_cols(it->lhs_col.tab_name, join_conds, index_col_names);
                if (index_exist) {
                    left_plan->tag = T_IndexScan;
                    left_plan->conds_ = join_conds;
                    left_plan->index_col_names_ = std::move(index_col_names);
                }
                index_col_names.clear();
            }

            // 检查右连接条件上是否有索引
            // 交换连接左右列
            auto right_conds = join_conds;
            std::swap(right_conds[0].lhs_col, right_conds[0].rhs_col);
            auto right_plan = std::dynamic_pointer_cast<ScanPlan>(right);
            if (right_plan->tag != T_IndexScan) {
                std::vector<std::string> index_col_names;
                bool index_exist = get_index_cols(it->rhs_col.tab_name, right_conds, index_col_names);
                if (index_exist) {
                    right_plan->tag = T_IndexScan;
                    right_plan->conds_ = std::move(right_conds);
                    right_plan->index_col_names_ = std::move(index_col_names);
                }
                index_col_names.clear();
            }

            // TODO 优化 sort 转索引
            if (x->has_sort) {
                if (left->tag != T_IndexScan && right->tag == T_IndexScan) {
                    // 为左列生成 sort
                    if (join_conds[0].lhs_col == query->sort_bys || join_conds[0].rhs_col == query->sort_bys) {
                        // TODO 检查排序列是否就是连接列，检查排序列上是否有索引
                        left = std::make_shared<SortPlan>(T_Sort, std::move(left), it->lhs_col,
                                                          x->order->orderby_dir == ast::OrderBy_DESC);
                        // 不用再生成 sort 算子来排序了
                        x->has_sort = false;
                    }
                } else if (left->tag == T_IndexScan && right->tag != T_IndexScan) {
                    // 为右列生成 sort
                    if (join_conds[0].lhs_col == query->sort_bys || join_conds[0].rhs_col == query->sort_bys) {
                        // TODO 检查排序列是否就是连接列，检查排序列上是否有索引
                        right = std::make_shared<SortPlan>(T_Sort, std::move(right), it->rhs_col,
                                                           x->order->orderby_dir == ast::OrderBy_DESC);
                        // 不用再生成 sort 算子来排序了
                        x->has_sort = false;
                    }
                } else if (left->tag != T_IndexScan && right->tag != T_IndexScan) {
                    if (join_conds[0].lhs_col == query->sort_bys || join_conds[0].rhs_col == query->sort_bys) {
                        // TODO 检查排序列是否就是连接列，检查排序列上是否有索引
                        left = std::make_shared<SortPlan>(T_Sort, std::move(left), it->lhs_col,
                                                          x->order->orderby_dir == ast::OrderBy_DESC);
                        right = std::make_shared<SortPlan>(T_Sort, std::move(right), it->rhs_col,
                                                           x->order->orderby_dir == ast::OrderBy_DESC);
                        // 不用再生成 sort 算子来排序了
                        x->has_sort = false;
                    }
                } else if (left->tag == T_IndexScan && right->tag == T_IndexScan) {
                    if (join_conds[0].lhs_col == query->sort_bys || join_conds[0].rhs_col == query->sort_bys) {
                        x->has_sort = false;
                    }
                }
            }

            // 建立join
            // 判断使用哪种join方式
            if (enable_nestedloop_join && enable_sortmerge_join) {
                // 默认nested loop join
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right),
                                                                  std::move(join_conds));
            } else if (enable_nestedloop_join) {
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right),
                                                                  std::move(join_conds));
            } else if (enable_sortmerge_join) {
                // 默认是 sortmerge，这里要判断是否真正满足
                if ((left->tag == T_Sort || left->tag == T_IndexScan) && (
                        right->tag == T_Sort || right->tag == T_IndexScan)) {
                    table_join_executors = std::make_shared<JoinPlan>(T_SortMerge, std::move(left), std::move(right),
                                                                      std::move(join_conds));
                } else {
                    // ticky 把嵌套连接变成归并连接来加速
                    // left = std::make_shared<SortPlan>(T_Sort, std::move(left), it->lhs_col,
                    //                                   false);
                    // right = std::make_shared<SortPlan>(T_Sort, std::move(right), it->rhs_col,
                    //                                    false);
                    table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right),
                                                                      std::move(join_conds));
                }
            } else {
                // error
                throw RMDBError("No join executor selected!");
            }

            // table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            it = conds.erase(it);
            break;
        }
        // 根据连接条件，生成第2-n层join
        it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left_need_to_join_executors = nullptr;
            std::shared_ptr<Plan> right_need_to_join_executors = nullptr;
            bool isneedreverse = false;
            if (std::find(joined_tables.begin(), joined_tables.end(), it->lhs_col.tab_name) == joined_tables.end()) {
                left_need_to_join_executors = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables,
                                                       table_scan_executors);
            }
            if (std::find(joined_tables.begin(), joined_tables.end(), it->rhs_col.tab_name) == joined_tables.end()) {
                right_need_to_join_executors = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables,
                                                        table_scan_executors);
                isneedreverse = true;
            }

            if (left_need_to_join_executors != nullptr && right_need_to_join_executors != nullptr) {
                std::vector<Condition> join_conds{*it};
                std::shared_ptr<Plan> temp_join_executors = std::make_shared<JoinPlan>(T_NestLoop,
                    std::move(left_need_to_join_executors),
                    std::move(right_need_to_join_executors),
                    join_conds);
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(temp_join_executors),
                                                                  std::move(table_join_executors),
                                                                  std::vector<Condition>());
            } else if (left_need_to_join_executors != nullptr || right_need_to_join_executors != nullptr) {
                if (isneedreverse) {
                    std::map<CompOp, CompOp> swap_op = {
                        {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
                    };
                    std::swap(it->lhs_col, it->rhs_col);
                    it->op = swap_op.at(it->op);
                    left_need_to_join_executors = std::move(right_need_to_join_executors);
                }
                std::vector<Condition> join_conds{*it};
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left_need_to_join_executors),
                                                                  std::move(table_join_executors), join_conds);
            } else {
                push_conds(std::move(&(*it)), table_join_executors);
            }
            it = conds.erase(it);
        }
    } else {
        table_join_executors = table_scan_executors[0];
        scantbl[0] = 1;
    }

    // 连接剩余表
    for (size_t i = 0; i < tables.size(); i++) {
        if (scantbl[i] == -1) {
            table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(table_scan_executors[i]),
                                                              std::move(table_join_executors),
                                                              std::vector<Condition>());
        }
    }

    return table_join_executors;
}

std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan) {
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if (!x->has_sort) {
        return plan;
    }
    // std::vector<ColMeta> all_cols;
    // for (auto &sel_tab_name: query->tables) {
    //     // 这里db_不能写成get_db(), 注意要传指针
    //     const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
    //     all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    // }
    // TabCol sel_col;
    // // TODO 支持多列排序
    // for (auto &col: all_cols) {
    //     if (col.name == x->order->cols->col_name) {
    //         sel_col = {.tab_name = col.tab_name, .col_name = col.name};
    //     }
    // }
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), std::move(query->sort_bys),
                                      x->order->orderby_dir == ast::OrderBy_DESC);
}

/**
 * @brief select plan 生成
 *
 * @param sel_cols select plan 选取的列
 * @param tab_names select plan 目标的表
 * @param conds select plan 选取条件
 */
std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context) {
    // 逻辑优化
    query = logical_optimization(std::move(query), context);

    // 物理优化
    auto sel_cols = query->cols;
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);

    // 检查是否是聚合语句，有 group 要走聚合
    bool is_agg = !query->group_bys.empty();
    if (!is_agg) {
        for (auto &agg_type: query->agg_types) {
            if (agg_type != AGG_COL) {
                is_agg = true;
                break;
            }
        }
    }

    // 生成聚合计划
    if (is_agg) {
        plannerRoot = std::make_shared<AggregatePlan>(T_Aggregate, std::move(plannerRoot), query->cols,
                                                      query->agg_types, query->group_bys, query->havings);
    }

    // TODO 待会处理别名
    plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot),
                                                   std::move(sel_cols), std::move(query->alias));

    return plannerRoot;
}

// 生成DDL语句和DML语句的查询执行计划
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context) {
    std::shared_ptr<Plan> plannerRoot;
    if (auto x = std::dynamic_pointer_cast<ast::CreateStaticCheckpoint>(query->parse)) {
        plannerRoot = std::make_shared<StaticCheckpointPlan>(T_CreateStaticCheckpoint);
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        // create table;
        std::vector<ColDef> col_defs;
        for (auto &field: x->fields) {
            if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
                ColDef col_def = {
                    .name = sv_col_def->col_name,
                    .type = interp_sv_type(sv_col_def->type_len->type),
                    .len = sv_col_def->type_len->len
                };
                col_defs.push_back(col_def);
            } else {
                throw InternalError("Unexpected field type");
            }
        }
        plannerRoot = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        // drop table;
        plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(),
                                                std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        // create index;
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        // drop index
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::LoadStmt>(query->parse)) {
        // load;
        plannerRoot = std::make_shared<LoadPlan>(T_Load, x->file_name, x->table_name);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        // insert;
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(), x->tab_name,
                                                query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        // delete;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {
            // 该表没有索引
            index_col_names.clear();
            table_scan_executors =
                    std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {
            // 存在索引
            table_scan_executors =
                    std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }

        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,
                                                std::vector<Value>(), query->conds, std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {
            // 该表没有索引
            index_col_names.clear();
            table_scan_executors =
                    std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {
            // 存在索引
            table_scan_executors =
                    std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                std::vector<Value>(), query->conds,
                                                query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {
        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
        // 子查询也只能有一个 DMLPlan，投影计划会有多个在 conds 里
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                std::vector<Condition>(), std::vector<SetClause>());
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}
