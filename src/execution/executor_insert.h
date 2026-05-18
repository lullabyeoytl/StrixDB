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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class InsertExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Value> values_;     // 需要插入的数据
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::string tab_name_;          // 表名称
    Rid rid_;                       // 插入的位置，在 Next() 中通过 next_insert_rid() 赋值后再写入记录
    SmManager *sm_manager_;

   public:
    InsertExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Value> values, Context *context) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        if (values.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;
        if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
        }
    };

    std::unique_ptr<RmRecord> Next() override {
        // Make record buffer
        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_.cols[i];
            Value val = coerce_value_to_type(values_[i], col.type);
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }
        // Pre-check all unique indexes before mutating anything.
        // This avoids dangling index entries when a table has multiple unique
        // indexes and a later one fails after earlier ones already succeeded.
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto &index = tab_.indexes[i];
            if (!index.unique) continue;
            auto ih = sm_manager_->get_ih(tab_name_, index.cols);
            auto key = std::make_unique<char[]>(index.col_tot_len);
            index.build_key(key.get(), rec.data);
            std::vector<Rid> result;
            if (ih->get_value(key.get(), &result, context_->txn_)) {
                throw UniqueViolationError(tab_name_, index.col_names());
            }
        }

        while (true) {
            rid_ = fh_->next_insert_rid();
            if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
                context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid_, fh_->GetFd());
            }
            // avoid insert duplicate slot
            if (!fh_->is_record(rid_)) {
                break;
            }
        }
        // WAL
        lsn_t op_prev_lsn = context_ != nullptr && context_->txn_ != nullptr ? context_->txn_->get_prev_lsn() : INVALID_LSN;
        lsn_t op_lsn = INVALID_LSN;
        if (context_ != nullptr && context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
            InsertLogRecord log_record(context_->txn_->get_transaction_id(), rec, rid_, tab_name_);
            log_record.prev_lsn_ = op_prev_lsn;
            op_lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
            context_->txn_->set_prev_lsn(op_lsn);
        }

        // Insert into record file
        fh_->insert_record(rid_, rec.data);
        if (op_lsn != INVALID_LSN) {
            fh_->set_page_lsn(rid_, op_lsn);
        }
        if (context_ != nullptr && context_->txn_ != nullptr) {
            context_->txn_->append_write_record(new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_, op_prev_lsn));
        }

        // Insert into index (uniqueness already pre-checked above)
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto &index = tab_.indexes[i];
            auto ih = sm_manager_->get_ih(tab_name_, index.cols);
            auto key = std::make_unique<char[]>(index.col_tot_len);
            index.build_key(key.get(), rec.data);
            ih->insert_entry(key.get(), rid_, context_->txn_);
        }
        return nullptr;
    }
    Rid &rid() override { return rid_; }
};
