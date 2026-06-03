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

#include <vector>

#include "common/context.h"
#include "transaction/transaction.h"
#include "transaction/transaction_manager.h"
#include "common/common.h"

inline bool is_mvcc_active(const Context *ctx) {
    return ctx != nullptr && TransactionManager::IsMvccActive(ctx->txn_);
}

inline void check_write_conflict(Context *ctx, int fd, const Rid &rid) {
    if (ctx != nullptr && ctx->txn_mgr_ != nullptr && TransactionManager::IsMvccActive(ctx->txn_)) {
        ctx->txn_mgr_->CheckWriteConflict(fd, rid, ctx->txn_);
    }
}

inline void track_ssi_predicate_read(Context *ctx, const std::string &table_name,
                                     const std::vector<Condition> &conds) {
    if (ctx != nullptr && ctx->txn_mgr_ != nullptr && TransactionManager::IsSerializableActive(ctx->txn_)) {
        ctx->txn_mgr_->TrackSsiPredicateRead(table_name, conds, ctx->txn_);
    }
}

inline void track_ssi_record_read(Context *ctx, const std::string &table_name,
                                  const std::vector<Condition> &conds, const Rid &rid,
                                  const RmRecord &rec) {
    if (ctx != nullptr && ctx->txn_mgr_ != nullptr && TransactionManager::IsSerializableActive(ctx->txn_)) {
        ctx->txn_mgr_->TrackSsiRecordRead(table_name, conds, rid, rec, ctx->txn_);
    }
}

inline void track_ssi_write(Context *ctx, const std::string &table_name, const Rid &rid,
                            const RmRecord *old_rec, const RmRecord *new_rec) {
    if (ctx != nullptr && ctx->txn_mgr_ != nullptr && TransactionManager::IsSerializableActive(ctx->txn_)) {
        // only ssi involved
        ctx->txn_mgr_->TrackSsiWrite(table_name, rid, old_rec, new_rec, ctx->txn_);
    }
}