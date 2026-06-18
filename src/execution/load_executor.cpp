#include "load_executor.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

#include "errors.h"
#include "index/ix.h"
#include "recovery/log_manager.h"
#include "transaction/transaction_manager.h"

namespace {

void write_load_output(const std::string &output, Context *context) {
    if (context == nullptr || context->data_send_ == nullptr || context->offset_ == nullptr) {
        return;
    }
    if (*(context->offset_) < 0) {
        *(context->offset_) = 0;
    }

    auto offset = static_cast<size_t>(*(context->offset_));
    if (offset >= BUFFER_LENGTH - 1) {
        *(context->offset_) = BUFFER_LENGTH - 1;
        context->data_send_[*(context->offset_)] = '\0';
        context->ellipsis_ = true;
        return;
    }

    size_t writable = BUFFER_LENGTH - 1 - offset;
    size_t write_len = std::min(writable, output.size());
    memcpy(context->data_send_ + offset, output.c_str(), write_len);
    *(context->offset_) += static_cast<int>(write_len);
    context->data_send_[*(context->offset_)] = '\0';
    if (write_len < output.size()) {
        context->ellipsis_ = true;
    }
}

bool load_rid_less(const Rid &lhs, const Rid &rhs) {
    if (lhs.page_no != rhs.page_no) {
        return lhs.page_no < rhs.page_no;
    }
    return lhs.slot_no < rhs.slot_no;
}

int compare_load_key(const IndexMeta &index, const char *lhs, const char *rhs) {
    int offset = 0;
    for (const auto &col : index.cols) {
        int result = ix_compare(lhs + offset, rhs + offset, col.type, col.len);
        if (result != 0) {
            return result;
        }
        offset += col.len;
    }
    return 0;
}

bool parse_load_int(std::string_view token, int *value) {
    if (token.empty()) {
        return false;
    }

    int sign = 1;
    size_t pos = 0;
    if (token[pos] == '+' || token[pos] == '-') {
        sign = token[pos] == '-' ? -1 : 1;
        pos++;
        if (pos == token.size()) {
            return false;
        }
    }

    long long parsed = 0;
    long long limit = sign < 0 ? static_cast<long long>(std::numeric_limits<int>::max()) + 1
                               : std::numeric_limits<int>::max();
    for (; pos < token.size(); ++pos) {
        unsigned char ch = static_cast<unsigned char>(token[pos]);
        if (ch < '0' || ch > '9') {
            return false;
        }
        parsed = parsed * 10 + (ch - '0');
        if (parsed > limit) {
            errno = ERANGE;
            return false;
        }
    }

    *value = static_cast<int>(sign * parsed);
    return true;
}

bool parse_load_float(std::string_view token, float *value) {
    std::string text(token);
    char *end = nullptr;
    errno = 0;
    float parsed = std::strtof(text.c_str(), &end);
    if (errno == ERANGE) {
        return false;
    }
    if (end != text.c_str() + text.size()) {
        return false;
    }
    *value = parsed;
    return true;
}

void write_load_field(std::string_view token, const ColMeta &col, char *record_data,
                      const std::string &file_name, int row_number, size_t col_number) {
    char *dest = record_data + col.offset;
    auto invalid_value = [&]() {
        throw RMDBError("Invalid value in file " + file_name +
                        " row " + std::to_string(row_number) +
                        " col " + std::to_string(col_number) +
                        ": '" + std::string(token) + "'");
    };
    auto out_of_range_value = [&]() {
        throw RMDBError("Value out of range in file " + file_name +
                        " row " + std::to_string(row_number) +
                        " col " + std::to_string(col_number) +
                        ": '" + std::string(token) + "'");
    };

    switch (col.type) {
        case TYPE_INT: {
            int parsed_value = 0;
            errno = 0;
            if (!parse_load_int(token, &parsed_value)) {
                if (errno == ERANGE) {
                    out_of_range_value();
                }
                invalid_value();
            }
            memcpy(dest, &parsed_value, sizeof(parsed_value));
            break;
        }
        case TYPE_FLOAT: {
            float parsed_value = 0;
            if (!parse_load_float(token, &parsed_value)) {
                if (errno == ERANGE) {
                    out_of_range_value();
                }
                invalid_value();
            }
            memcpy(dest, &parsed_value, sizeof(parsed_value));
            break;
        }
        case TYPE_STRING:
            if (static_cast<int>(token.size()) > col.len) {
                throw StringOverflowError();
            }
            memset(dest, 0, col.len);
            memcpy(dest, token.data(), token.size());
            break;
        case TYPE_DATETIME: {
            int64_t parsed_value = datetime_literal_to_seconds(std::string(token));
            memcpy(dest, &parsed_value, sizeof(parsed_value));
            break;
        }
        default:
            throw InternalError("Unexpected column type in LOAD");
    }
}

struct LoadKeyEntry {
    size_t key_offset;
    Rid rid;
};

struct LoadIndexState {
    IndexMeta *meta;
    IxIndexHandle *handle;
    std::vector<char> key_storage;
    std::vector<LoadKeyEntry> entries;
    bool sorted = true;

    char *key_data(LoadKeyEntry &entry) { return key_storage.data() + entry.key_offset; }

    const char *key_data(const LoadKeyEntry &entry) const { return key_storage.data() + entry.key_offset; }
};

bool load_entry_less(const LoadIndexState &index, const LoadKeyEntry &lhs, const LoadKeyEntry &rhs) {
    int result = compare_load_key(*index.meta, index.key_data(lhs), index.key_data(rhs));
    if (result != 0) {
        return result < 0;
    }
    return load_rid_less(lhs.rid, rhs.rid);
}

void rollback_load_index_entries(std::vector<LoadIndexState> &indexes, size_t finished_indexes,
                                 size_t current_index, size_t current_entries, Transaction *txn) {
    auto delete_entries = [&](size_t index_pos, size_t count) {
        auto &state = indexes[index_pos];
        for (size_t entry_pos = count; entry_pos > 0; --entry_pos) {
            auto &entry = state.entries[entry_pos - 1];
            state.handle->delete_entry(state.key_data(entry), entry.rid, txn);
        }
    };

    if (current_index < indexes.size()) {
        delete_entries(current_index, current_entries);
    }
    for (size_t index_pos = finished_indexes; index_pos > 0; --index_pos) {
        delete_entries(index_pos - 1, indexes[index_pos - 1].entries.size());
    }
}

size_t estimate_load_rows(std::ifstream &csv_file, int record_size) {
    auto original_pos = csv_file.tellg();
    csv_file.seekg(0, std::ios::end);
    auto end_pos = csv_file.tellg();
    csv_file.seekg(original_pos, std::ios::beg);
    if (end_pos <= 0 || record_size <= 0) {
        return 0;
    }
    return static_cast<size_t>(end_pos) / static_cast<size_t>(record_size);
}

}  // namespace

void execute_load_plan(const LoadPlan &plan, SmManager *sm_manager, Context *context) {
    auto &tab = sm_manager->db_.get_table(plan.tab_name_);
    auto fh = sm_manager->fhs_.at(plan.tab_name_).get();
    int record_size = fh->get_file_hdr().record_size;

    std::ifstream csv_file(plan.file_name_);
    if (!csv_file.is_open()) {
        throw RMDBError("Cannot open file: " + plan.file_name_);
    }

    size_t estimated_rows = estimate_load_rows(csv_file, record_size);

    std::string line;
    if (!std::getline(csv_file, line)) {
        throw RMDBError("File is empty: " + plan.file_name_);
    }

    if (context != nullptr && context->lock_mgr_ != nullptr) {
        context->lock_mgr_->lock_IX_on_table(context->txn_, fh->GetFd());
        context->lock_mgr_->lock_exclusive_on_table(context->txn_, fh->GetFd());
    }

    auto write_guard = context != nullptr && context->txn_mgr_ != nullptr
                           ? context->txn_mgr_->write_txn_guard()
                           : TransactionManager::WriteTxnGuard(nullptr);

    std::vector<LoadIndexState> load_indexes;
    load_indexes.reserve(tab.indexes.size());
    for (auto &index : tab.indexes) {
        load_indexes.push_back(LoadIndexState{&index, sm_manager->get_ih(plan.tab_name_, index.cols), {}, {}, true});
        load_indexes.back().entries.reserve(estimated_rows);
        load_indexes.back().key_storage.reserve(estimated_rows * static_cast<size_t>(index.col_tot_len));
    }

    RmRecord rec(record_size);
    std::vector<std::string_view> tokens;
    tokens.reserve(tab.cols.size());

    WriteRecord *load_write_record = nullptr;
    if (context != nullptr && context->txn_ != nullptr) {
        load_write_record = new WriteRecord(WType::INSERT_TUPLE_BATCH, plan.tab_name_);
        load_write_record->GetBatchRids().reserve(estimated_rows);
        context->txn_->append_write_record(load_write_record);
    }

    int row_count = 0;
    {
        auto bulk_insert = fh->start_bulk_insert();
        while (std::getline(csv_file, line)) {
            if (line.empty()) {
                continue;
            }

            tokens.clear();
            size_t field_start = 0;
            for (size_t pos = 0; pos < line.size(); ++pos) {
                if (line[pos] == ',') {
                    tokens.emplace_back(line.data() + field_start, pos - field_start);
                    field_start = pos + 1;
                }
            }
            tokens.emplace_back(line.data() + field_start, line.size() - field_start);

            if (tokens.size() != tab.cols.size()) {
                throw RMDBError("Column count mismatch in file " + plan.file_name_ +
                                ": expected " + std::to_string(tab.cols.size()) +
                                ", got " + std::to_string(tokens.size()));
            }

            for (size_t i = 0; i < tokens.size(); i++) {
                write_load_field(tokens[i], tab.cols[i], rec.data, plan.file_name_, row_count + 1, i + 1);
            }

            lsn_t op_prev_lsn = context != nullptr && context->txn_ != nullptr
                                    ? context->txn_->get_prev_lsn()
                                    : INVALID_LSN;
            Rid rid = bulk_insert->insert(rec.data, [&](const Rid &next_rid) {
                lsn_t op_lsn = INVALID_LSN;
                if (context != nullptr && context->txn_ != nullptr && context->log_mgr_ != nullptr) {
                    InsertLogRecordView log_record(context->txn_->get_transaction_id(), rec.data, rec.size, next_rid,
                                                   plan.tab_name_);
                    log_record.prev_lsn_ = op_prev_lsn;
                    op_lsn = context->log_mgr_->add_log_to_buffer_relaxed(&log_record);
                    context->txn_->set_prev_lsn(op_lsn);
                }
                return op_lsn;
            });

            if (load_write_record != nullptr) {
                load_write_record->AppendBatchRid(rid, op_prev_lsn);
            }

            for (auto &index : load_indexes) {
                LoadKeyEntry entry{index.key_storage.size(), rid};
                index.key_storage.resize(entry.key_offset + index.meta->col_tot_len);
                index.meta->build_key(index.key_data(entry), rec.data);
                if (index.sorted && !index.entries.empty() && load_entry_less(index, entry, index.entries.back())) {
                    index.sorted = false;
                }
                index.entries.push_back(entry);
            }
            row_count++;
        }
    }

    for (auto &index : load_indexes) {
        if (!index.sorted) {
            std::sort(index.entries.begin(), index.entries.end(), [&](const LoadKeyEntry &lhs,
                                                                      const LoadKeyEntry &rhs) {
                return load_entry_less(index, lhs, rhs);
            });
        }
    }

    Transaction *txn = context != nullptr ? context->txn_ : nullptr;
    for (auto &index : load_indexes) {
        if (!index.meta->unique) {
            continue;
        }

        for (size_t entry_pos = 1; entry_pos < index.entries.size(); ++entry_pos) {
            const auto &prev = index.entries[entry_pos - 1];
            const auto &entry = index.entries[entry_pos];
            if (compare_load_key(*index.meta, index.key_data(prev), index.key_data(entry)) == 0) {
                throw UniqueViolationError(plan.tab_name_, index.meta->col_names());
            }
        }
    }

    size_t finished_indexes = 0;
    size_t current_index = load_indexes.size();
    size_t current_entries = 0;
    try {
        for (current_index = 0; current_index < load_indexes.size(); ++current_index) {
            auto &index = load_indexes[current_index];
            current_entries = 0;

            std::vector<IxBulkLoadEntry> bulk_entries;
            bulk_entries.reserve(index.entries.size());
            for (const auto &entry : index.entries) {
                bulk_entries.push_back(IxBulkLoadEntry{index.key_data(entry), entry.rid});
            }
            bool bulk_loaded = false;
            if (context != nullptr && context->lock_mgr_ != nullptr) {
                bulk_loaded = index.handle->bulk_load_sorted_entries(bulk_entries, txn);
            }
            if (bulk_loaded) {
                current_entries = index.entries.size();
            } else {
                for (const auto &entry : index.entries) {
                    try {
                        index.handle->insert_entry(index.key_data(entry), entry.rid, txn, nullptr,
                                                   InsertEntryOptions::SkipUniqueProbe);
                    } catch (const UniqueKeyViolationError &) {
                        throw UniqueViolationError(plan.tab_name_, index.meta->col_names());
                    }
                    ++current_entries;
                }
            }
            ++finished_indexes;
        }
    } catch (...) {
        rollback_load_index_entries(load_indexes, finished_indexes, current_index, current_entries, txn);
        throw;
    }

    write_load_output("Loaded " + std::to_string(row_count) + " rows into " + plan.tab_name_ + "\n", context);
}
