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
#include <string>
#include <vector>

#include "execution/execution_defs.h"
#include "execution/execution_manager.h"
#include "record/rm.h"
#include "system/sm.h"
#include "common/context.h"
#include "plan.h"
#include "parser/parser.h"
#include "common/common.h"
#include "analyze/analyze.h"

class Planner {
private:
    SmManager *sm_manager_;

    bool enable_nestedloop_join = false;
    bool enable_sortmerge_join = true;

public:
    Planner(SmManager *sm_manager) : sm_manager_(sm_manager) {
    }


    std::shared_ptr<Plan> do_planner(std::shared_ptr<Query> query, Context *context);

    void set_enable_nestedloop_join(bool set_val) { enable_nestedloop_join = set_val; }

    void set_enable_sortmerge_join(bool set_val) { enable_sortmerge_join = set_val; }

    void set_enable_output_file(bool set_val) { enable_output_file = set_val; }

    // 是否把输入写入 output.txt 文件中，默认开启
    bool enable_output_file = false;

private:
    std::shared_ptr<Query> logical_optimization(std::shared_ptr<Query> query, Context *context);

    std::shared_ptr<Plan> physical_optimization(std::shared_ptr<Query> query, Context *context);

    std::shared_ptr<Plan> make_one_rel(std::shared_ptr<Query> query, Context *context);

    std::shared_ptr<Plan> generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan);

    std::shared_ptr<Plan> generate_select_plan(std::shared_ptr<Query> query, Context *context);

    std::shared_ptr<Plan> pop_scan(int *scantbl, const std::string &table, std::vector<std::string> &joined_tables,
                                   std::vector<std::shared_ptr<Plan> > plans);

    std::vector<Condition> pop_conds(std::vector<Condition> &conds, const std::string &tab_names, Context *context);

    // int get_indexNo(std::string tab_name, std::vector<Condition> curr_conds);
    bool get_index_cols(std::string &tab_name, std::vector<Condition> &curr_conds,
                        std::vector<std::string> &index_col_names);

    ColType interp_sv_type(ast::SvType sv_type) {
        std::map<ast::SvType, ColType> m = {
            {ast::SV_TYPE_INT, TYPE_INT}, {ast::SV_TYPE_FLOAT, TYPE_FLOAT}, {ast::SV_TYPE_STRING, TYPE_STRING}
        };
        return m.at(sv_type);
    }
};
