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

IxScan::IxScan(const IxIndexHandle *ih, const Iid &lower, ScanUpperBound upper_bound, BufferPoolManager *bpm)
    : ih_(ih), iid_(lower), upper_bound_(std::move(upper_bound)), bpm_(bpm) {
    if (iid_.page_no == INVALID_PAGE_ID) {
        is_end_ = true;
        return;
    }

    current_node_ = ih_->fetch_node(iid_.page_no);
    current_node_->RLatch();
    advance_to_valid_record();
}

IxScan::~IxScan() {
    release_current();
}

void IxScan::release_current() {
    if (!current_node_) {
        return;
    }
    current_node_->RUnlatch();
    bpm_->unpin_page(current_node_->get_page_id(), false);
    current_node_.reset();
}

void IxScan::move_to_next_leaf() {
    assert(current_node_ != nullptr);
    assert(current_node_->is_leaf_page());

    if (!current_node_->has_next()) {
        release_current();
        iid_ = Iid{INVALID_PAGE_ID, -1};
        is_end_ = true;
        return;
    }

    auto next = ih_->fetch_node(current_node_->get_next());
    next->RLatch();
    release_current();
    current_node_ = std::move(next);
    iid_.page_no = current_node_->get_page_no();
    iid_.slot_no = 0;
}

// need outer S-latch protection
bool IxScan::exceeds_upper_bound() const {
    if (!upper_bound_.has_bound) {
        return false;
    }

    int cmp = ix_compare(current_node_->get_key(iid_.slot_no), upper_bound_.key.data(),
                         ih_->file_hdr_->col_types_, ih_->file_hdr_->col_lens_);
    return upper_bound_.inclusive ? (cmp > 0) : (cmp >= 0);
}

void IxScan::advance_to_valid_record() {
    while (current_node_) {
        assert(current_node_->is_leaf_page());

        while (iid_.slot_no >= current_node_->get_size()) {
            if (!current_node_->has_next()) {
                release_current();
                iid_ = Iid{INVALID_PAGE_ID, -1};
                is_end_ = true;
                return;
            }
            move_to_next_leaf();
        }

        if (exceeds_upper_bound()) {
            release_current();
            iid_ = Iid{INVALID_PAGE_ID, -1};
            is_end_ = true;
            return;
        }

        is_end_ = false;
        iid_.page_no = current_node_->get_page_no();
        return;
    }

    iid_ = Iid{INVALID_PAGE_ID, -1};
    is_end_ = true;
}

void IxScan::next() {
    assert(!is_end());
    iid_.slot_no++;
    advance_to_valid_record();
}

Rid IxScan::rid() const {
    assert(!is_end_);
    assert(current_node_ != nullptr);
    return *current_node_->get_rid(iid_.slot_no);
}
