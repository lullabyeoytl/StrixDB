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

#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::string exposed_name_;
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    bool empty_result_ = false;

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator

    SmManager *sm_manager_;

    void set_end() {
        rid_ = Rid{-1, -1};
    }

    // add filter conds
    void seek_to_next_valid() {
        if (!seek_to_next_valid_tuple(scan_.get(), rid_, conds_, cols_, [&](const Rid &rid) {
                if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
                    context_->lock_mgr_->lock_shared_on_record(context_->txn_, rid, fh_->GetFd());
                }
                return fh_->get_record(rid, context_);
            })) {
            set_end();
        }
    }

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                    std::string exposed_name, bool empty_result, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        exposed_name_ = std::move(exposed_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        for (auto &col : cols_) {
            col.tab_name = exposed_name_;
        }
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;
        if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_IS_on_table(context_->txn_, fh_->GetFd());
        }
        empty_result_ = empty_result;
        set_end();
    }

    void beginTuple() override {
        if (empty_result_) {
            scan_.reset();
            set_end();
            return;
        }
        scan_ = std::make_unique<RmScan>(fh_);
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

    // Expose the scan kind so upper DML executors can choose their row consumption strategy.
    std::string getType() override { return "SeqScanExecutor"; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

};
