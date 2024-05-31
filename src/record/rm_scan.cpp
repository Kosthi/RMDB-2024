/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // Todo:
    // 初始化file_handle和rid（指向第一个存放了记录的位置）
    rid_ = {RM_FIRST_RECORD_PAGE, -1};
    // 这里设置-1，Bit::next_bit即是0，直接设置为0，会少判断0
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // Todo:
    // 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置
    while (rid_.page_no < file_handle_->file_hdr_.num_pages) {
        auto &&rm_page_handle = file_handle_->fetch_page_handle(rid_.page_no);
        rid_.slot_no = Bitmap::next_bit(true, rm_page_handle.bitmap, file_handle_->file_hdr_.num_records_per_page,
                                        rid().slot_no);
        // 一定要 unpin，否则多次 scan 以后所有页面都会无法替换！
        file_handle_->buffer_pool_manager_->unpin_page(rm_page_handle.page->get_page_id(), false);
        if (rid_.slot_no < file_handle_->file_hdr_.num_records_per_page) {
            return;
        }
        ++rid_.page_no;
        rid_.slot_no = -1;
    }
    rid_.page_no = RM_NO_PAGE;
}

/**
 * @brief 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // Todo: 修改返回值
    return rid_.page_no == RM_NO_PAGE;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}
