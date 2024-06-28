/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_scan.h"

/**
 * @brief
 * @todo 加上读锁（需要使用缓冲池得到page）
 */
void IxScan::next() {
    assert(!is_end());
    auto &&node = ih_->fetch_node(iid_.page_no);
    assert(node->is_leaf_page());
    assert(iid_.slot_no < node->get_size());
    // increment slot no
    iid_.slot_no++;
    if (iid_.page_no != ih_->file_hdr_->last_leaf_ && iid_.slot_no == node->get_size()) {
        // go to next leaf
        iid_.slot_no = 0;
        iid_.page_no = node->get_next_leaf();
    }
    // unpin page! 否则多次大量扫描读会出问题
    bpm_->unpin_page(node->page->get_page_id(), false);
}

Rid IxScan::rid() const {
    return ih_->get_rid(iid_);
}

Iid IxScan::prev_iid() {
    auto slot = iid_.slot_no;
    if (--slot >= 0) {
        return {iid_.page_no, slot};
    }
    auto &&node = ih_->fetch_node(iid_.page_no);
    assert(node->is_leaf_page());
    if (node->get_page_no() != ih_->file_hdr_->first_leaf_) {
        auto &&prev_node = ih_->fetch_node(node->get_prev_leaf());
        Iid iid = {prev_node->get_page_no(), prev_node->get_size() - 1};
        bpm_->unpin_page(prev_node->get_page_id(), false);
        bpm_->unpin_page(node->get_page_id(), false);
        return iid;
    }
    bpm_->unpin_page(node->get_page_id(), false);
    return {-1, -1};
}

Iid IxScan::prev_iid(const Iid &iid) {
    auto slot = iid.slot_no;
    if (--slot >= 0) {
        return {iid.page_no, slot};
    }
    auto &&node = ih_->fetch_node(iid_.page_no);
    assert(node->is_leaf_page());
    if (node->get_page_no() != ih_->file_hdr_->first_leaf_) {
        auto &&prev_node = ih_->fetch_node(node->get_prev_leaf());
        Iid iidd = {prev_node->get_page_no(), prev_node->get_size() - 1};
        bpm_->unpin_page(prev_node->get_page_id(), false);
        bpm_->unpin_page(node->get_page_id(), false);
        return iidd;
    }
    bpm_->unpin_page(node->get_page_id(), false);
    return {-1, -1};
}

RmRecord IxScan::get_key() {
    return ih_->get_key(iid_);
}
