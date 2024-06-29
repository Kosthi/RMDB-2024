#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class LoadExecutor : public AbstractExecutor {
private:
    TabMeta tab_; // 表的元数据
    // std::vector<Value> values_; // 需要插入的数据
    RmFileHandle *fh_; // 表的数据文件句柄
    std::string filename_; // 载入文件名
    std::string tab_name_; // 表名称
    Rid rid_; // 插入的位置，由于系统默认插入时不指定位置，因此当前rid_在插入后才赋值
    SmManager *sm_manager_;

public:
    LoadExecutor(SmManager *sm_manager, std::string &filename, std::string &tab_name,
                 Context *context) : filename_(std::move(filename)), tab_name_(std::move(tab_name)) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        context_ = context;

        // IX 锁
        // if (context_ != nullptr) {
        //     context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
        // }
    }

    std::unique_ptr<RmRecord> Next() override {
        printf("load success file: %s, tab: %s\n", filename_.c_str(), tab_name_.c_str());
        return nullptr;
    }

    Rid &rid() override { return rid_; }
};
