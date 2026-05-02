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

#include <cstring>
#include <algorithm>
#include <climits>
#include <cfloat>

#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

    void set_end() {
        rid_ = Rid{-1, -1};
    }

    void seek_to_next_valid() {
        if (!scan_ || scan_->is_end()) {
            set_end();
            return;
        }
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto record = fh_->get_record(rid_, context_);
            if (evaluate_conditions(fed_conds_, *record, cols_)) {
                return;
            }
            scan_->next();
        }
        set_end();
    }

    // 为索引构建搜索 key。当 for_upper=true 时，没有 EQ 匹配的列用类型最大值填充，
    // 使得 upper_bound 能正确找到首个大于所有匹配行的位置（复合索引只匹配前导列时）。
    std::vector<char> build_index_key(bool for_upper = false) {
        std::vector<char> key(index_meta_.col_tot_len, 0);
        int offset = 0;
        for (const auto &index_col : index_meta_.cols) {
            bool found = false;
            for (auto &cond : conds_) {
                if (cond.lhs_col.col_name == index_col.name && cond.is_rhs_val && cond.op == OP_EQ) {
                    if (!cond.rhs_val.raw) {
                        cond.rhs_val.init_raw(index_col.len);
                    }
                    memcpy(key.data() + offset, cond.rhs_val.raw->data, index_col.len);
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (for_upper) {
                    switch (index_col.type) {
                        case TYPE_INT: {
                            int val = INT_MAX;
                            memcpy(key.data() + offset, &val, sizeof(int));
                            break;
                        }
                        case TYPE_FLOAT: {
                            float val = FLT_MAX;
                            memcpy(key.data() + offset, &val, sizeof(float));
                            break;
                        }
                        case TYPE_STRING:
                            memset(key.data() + offset, 0xFF, index_col.len);
                            break;
                    }
                } else {
                    switch (index_col.type) {
                        case TYPE_INT: {
                            int val = INT_MIN;
                            memcpy(key.data() + offset, &val, sizeof(int));
                            break;
                        }
                        case TYPE_FLOAT: {
                            float val = -FLT_MAX;
                            memcpy(key.data() + offset, &val, sizeof(float));
                            break;
                        }
                        case TYPE_STRING:
                            break;
                    }
                }
            }
            offset += index_col.len;
        }
        return key;
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = std::move(index_col_names); 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
        set_end();
    }

    void beginTuple() override {
        auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        auto *ih = sm_manager_->ihs_.at(ix_name).get();

        auto lower_key = build_index_key(false);
        auto upper_key = build_index_key(true);
        Iid lower = ih->lower_bound(lower_key.data());
        Iid upper = ih->upper_bound(upper_key.data());
        scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());
        seek_to_next_valid();
    }

    void nextTuple() override {
        if (!scan_ || scan_->is_end()) {
            set_end();
            return;
        }
        scan_->next();
        seek_to_next_valid();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override {
        return rid_.page_no == -1 && rid_.slot_no == -1;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};