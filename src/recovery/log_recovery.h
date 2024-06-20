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

#include <unordered_map>
#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"

class TransactionManager;

class RedoLogsInPage {
public:
    RedoLogsInPage() { table_file_ = nullptr; }
    RmFileHandle *table_file_;
    std::vector<lsn_t> redo_logs_; // 在该page上需要redo的操作的lsn
};

class RecoveryManager {
public:
    RecoveryManager(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, SmManager *sm_manager,
                    LogManager *log_manager, TransactionManager *transaction_manager) : disk_manager_(disk_manager),
        buffer_pool_manager_(buffer_pool_manager), sm_manager_(sm_manager),
        log_manager_(log_manager), transaction_manager_(transaction_manager), transaction_(666) {
    }

    void analyze();

    void redo();

    void undo();

    void redo_indexes();

private:
    LogBuffer buffer_; // 读入日志
    DiskManager *disk_manager_; // 用来读写文件
    BufferPoolManager *buffer_pool_manager_; // 对页面进行读写
    SmManager *sm_manager_; // 访问数据库元数据
    LogManager *log_manager_; // 维护日志 global_lsn_ 至最新
    TransactionManager *transaction_manager_; // 维护 next_txn_id_ 至最新
    /** Maintain active transactions and its corresponding latest lsn. */
    std::unordered_map<txn_id_t, lsn_t> active_txn_;
    /** Mapping the log sequence number to log file offset for undos. */
    std::unordered_map<lsn_t, int> lsn_mapping_;
    /** DPT for redo. */
    std::deque<lsn_t> dirty_page_table_;
    Transaction transaction_;
    bool is_need_redo_indexes{false};
};
