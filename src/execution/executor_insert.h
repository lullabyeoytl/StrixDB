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

    std::unique_ptr<RmRecord> NextImpl() override {
        auto write_guard = context_ != nullptr && context_->txn_mgr_ != nullptr
                               ? context_->txn_mgr_->write_txn_guard()
                               : TransactionManager::WriteTxnGuard(nullptr);
        // Make record buffer
        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_.cols[i];
            Value val = coerce_value_to_type(values_[i], col.type);
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
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

        std::vector<std::pair<IndexMeta *, std::unique_ptr<char[]>>> inserted_keys;
        std::vector<std::string> violation_cols;
        bool version_link_prepared = false;
        bool heap_inserted = false;
        auto cleanup_insert_attempt = [&]() {
            for (auto it = inserted_keys.rbegin(); it != inserted_keys.rend(); ++it) {
                auto ih = sm_manager_->get_ih(tab_name_, it->first->cols);
                ih->delete_entry(it->second.get(), rid_, context_->txn_);
            }
            if (heap_inserted) {
                try {
                    fh_->delete_record(rid_, nullptr);
                } catch (const RecordNotFoundError &) {
                }
            }
            if (version_link_prepared && context_ != nullptr && context_->txn_mgr_ != nullptr &&
                context_->txn_ != nullptr) {
                txn_id_t txn_id = context_->txn_->get_transaction_id();
                context_->txn_mgr_->UpdateVersionLink(
                    fh_->GetFd(), rid_, std::nullopt,
                    [txn_id](std::optional<VersionUndoLink> current) {
                        return current.has_value() && current->prev_.prev_txn_ == txn_id;
                    });
            }
            fh_->release_reserved_rid(rid_);
        };
        try {
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto ih = sm_manager_->get_ih(tab_name_, index.cols);
                auto key = std::make_unique<char[]>(index.col_tot_len);
                index.build_key(key.get(), rec.data);
                try {
                    ih->insert_entry(key.get(), rid_, context_->txn_);
                } catch (const UniqueKeyViolationError &) {
                    violation_cols = index.col_names();
                    throw;
                }
                inserted_keys.emplace_back(&index, std::move(key));
            }

            track_ssi_write(context_, tab_name_, rid_, nullptr, &rec);
            if (context_ != nullptr && context_->txn_mgr_ != nullptr && context_->txn_ != nullptr) {
                context_->txn_mgr_->PrepareInsert(fh_->GetFd(), rid_, rec, context_->txn_);
                version_link_prepared = TransactionManager::IsMvccActive(context_->txn_);
            }

            lsn_t op_prev_lsn =
                context_ != nullptr && context_->txn_ != nullptr ? context_->txn_->get_prev_lsn() : INVALID_LSN;
            lsn_t op_lsn = INVALID_LSN;
            if (context_ != nullptr && context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
                InsertLogRecord log_record(context_->txn_->get_transaction_id(), rec, rid_, tab_name_);
                log_record.prev_lsn_ = op_prev_lsn;
                op_lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                context_->txn_->set_prev_lsn(op_lsn);
            }

            fh_->insert_record(rid_, rec.data, op_lsn);
            heap_inserted = true;
            if (context_ != nullptr && context_->txn_ != nullptr) {
                context_->txn_->append_write_record(new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_, op_prev_lsn));
            }
        } catch (const UniqueKeyViolationError &) {
            cleanup_insert_attempt();
            throw UniqueViolationError(tab_name_, violation_cols);
        } catch (...) {
            cleanup_insert_attempt();
            throw;
        }
        return nullptr;
    }
    Rid &rid() override { return rid_; }
};
