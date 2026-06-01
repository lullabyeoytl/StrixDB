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

#include "ix_defs.h"
#include "ix_index_handle.h"

// class IxIndexHandle;

// 用于遍历叶子结点
// 用于直接遍历叶子结点，而不用findleafpage来得到叶子结点
// TODO：对page遍历时，要加上读锁
class IxScan : public RecScan {
    const IxIndexHandle *ih_;
    Iid iid_;  // 初始为lower（用于遍历的指针）
    ScanUpperBound upper_bound_;
    std::unique_ptr<IxNodeHandle> current_node_;
    BufferPoolManager *bpm_;
    bool is_end_ = false;

   public:
    IxScan(const IxIndexHandle *ih, const Iid &lower, ScanUpperBound upper_bound, BufferPoolManager *bpm);

    ~IxScan() override;

    void next() override;

    bool is_end() const override { return is_end_; }

    Rid rid() const override;

    const char *key() const;

    const Iid &iid() const { return iid_; }

   private:
    void release_current();

    void move_to_next_leaf();

    void advance_to_valid_record();

    bool exceeds_upper_bound() const;
};
