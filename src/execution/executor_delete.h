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
#include <utility>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class DeleteExecutor : public AbstractExecutor {
private:
    TabMeta tab_; // 表的元数据
    std::vector<Condition> conds_; // delete的条件
    RmFileHandle *fh_; // 表的数据文件句柄
    std::vector<Rid> rids_; // 需要删除的记录的位置
    std::string tab_name_; // 表名称
    SmManager *sm_manager_;

public:
    DeleteExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context): sm_manager_(sm_manager), tab_name_(std::move(tab_name)),
                                                             conds_(std::move(conds)),
                                                             rids_(std::move(rids)) {
        tab_ = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        context_ = context;
    }

    // 只执行一次
    std::unique_ptr<RmRecord> Next() override {
        for (auto &rid: rids_) {
            auto &&rec = fh_->get_record(rid, context_);
            // 如果有索引，则必然是唯一索引
            for (auto &[index_name, index] : tab_.indexes) {
                auto &&ih = sm_manager_->ihs_.at(index_name).get();
                char *key = new char[index.col_tot_len];
                int offset = 0;
                for (size_t i = 0; i < index.col_num; ++i) {
                    memcpy(key + offset, rec->data + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                ih->delete_entry(key, context_->txn_);
                delete []key;
            }
            fh_->delete_record(rid, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
