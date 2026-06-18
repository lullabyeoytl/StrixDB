/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "execution_manager.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

#include "execution_common.h"
#include "executor_delete.h"
#include "executor_index_scan.h"
#include "executor_insert.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_seq_scan.h"
#include "executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

namespace {

auto indent(int depth) -> std::string {
    return std::string(static_cast<size_t>(depth), '\t');
}

void append_executor_tree(const AbstractExecutor *executor, int depth, std::string &out) {
    if (executor == nullptr) {
        return;
    }
    if (depth >= kMaxPlanTreeDepth) {
        throw InternalError("Executor tree depth exceeds limit");
    }

    std::string name = executor->explain_name();
    out += indent(depth) + name + "(";
    if (!executor->explain_attrs().empty()) {
        out += executor->explain_attrs() + ", ";
    }
    out += "rows=" + std::to_string(executor->rows()) + ")\n";

    auto children = executor->children();
    if (name != "Join") {
        std::stable_sort(children.begin(), children.end(),
                         [](const AbstractExecutor *lhs, const AbstractExecutor *rhs) {
                             auto rank = [](const AbstractExecutor *node) {
                                 if (node == nullptr) {
                                     return 5;
                                 }
                                 const auto &name = node->explain_name();
                                 if (name == "Filter") {
                                     return 0;
                                 }
                                 if (name == "Join") {
                                     return 1;
                                 }
                                 if (name == "Project") {
                                     return 2;
                                 }
                                 if (name == "Scan") {
                                     return 3;
                                 }
                                 return 4;
                             };
                             return rank(lhs) < rank(rhs);
                         });
    }

    for (const auto *child : children) {
        append_executor_tree(child, depth + 1, out);
    }
}

void write_bounded_output(const std::string &output, Context *context) {
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

}  // namespace

const char *help_info = "Supported SQL syntax:\n"
                   "  command ;\n"
                   "command:\n"
                   "  CREATE TABLE table_name (column_name type [, column_name type ...])\n"
                   "  DROP TABLE table_name\n"
                   "  CREATE INDEX table_name (column_name)\n"
                   "  SHOW INDEX FROM table_name\n"
                   "  DROP INDEX table_name (column_name)\n"
                   "  INSERT INTO table_name VALUES (value [, value ...])\n"
                   "  LOAD file_name INTO table_name\n"
                   "  DELETE FROM table_name [WHERE where_clause]\n"
                   "  UPDATE table_name SET column_name = value [, column_name = value ...] [WHERE where_clause]\n"
                   "  SELECT selector FROM table_name [WHERE where_clause]\n"
                   "type:\n"
                   "  {INT | FLOAT | CHAR(n) | DATETIME}\n"
                   "where_clause:\n"
                   "  condition [AND condition ...]\n"
                   "condition:\n"
                   "  column op {column | value}\n"
                   "column:\n"
                   "  [table_name.]column_name\n"
                   "op:\n"
                   "  {= | <> | < | > | <= | >=}\n"
                   "selector:\n"
                   "  {* | column [, column ...]}\n";

// 主要负责执行DDL语句
void QlManager::run_mutli_query(std::shared_ptr<Plan> plan, Context *context){
    if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
        switch(x->tag) {
            case T_CreateTable:
            {
                sm_manager_->create_table(x->tab_name_, x->cols_, context);
                for (auto &spec : x->index_specs_) {
                    sm_manager_->create_index(x->tab_name_, spec.cols, context);
                }
                break;
            }
            case T_DropTable:
            {
                sm_manager_->drop_table(x->tab_name_, context);
                break;
            }
            case T_CreateIndex:
            {
                sm_manager_->create_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            case T_DropIndex:
            {
                sm_manager_->drop_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;
        }
    }
}

// 执行help; show tables; desc table; begin; commit; abort;语句
void QlManager::run_cmd_utility(std::shared_ptr<Plan> plan, txn_id_t *txn_id, Context *context) {
    if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
        switch(x->tag) {
            case T_Help:
            {
                memcpy(context->data_send_ + *(context->offset_), help_info, strlen(help_info));
                *(context->offset_) = strlen(help_info);
                break;
            }
            case T_ShowTable:
            {
                sm_manager_->show_tables(context);
                break;
            }
            case T_ShowIndex:
            {
                sm_manager_->show_index(x->tab_name_, context);
                break;
            }
            case T_DescTable:
            {
                sm_manager_->desc_table(x->tab_name_, context);
                break;
            }
            case T_Transaction_begin:
            {
                // 显示开启一个事务
                context->txn_->set_txn_mode(true);
                break;
            }
            case T_Transaction_commit:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->commit(context->txn_, context->log_mgr_);
                context->txn_ = nullptr;
                *txn_id = INVALID_TXN_ID;
                break;
            }
            case T_Transaction_rollback:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                context->txn_ = nullptr;
                *txn_id = INVALID_TXN_ID;
                break;
            }
            case T_Transaction_abort:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                context->txn_ = nullptr;
                *txn_id = INVALID_TXN_ID;
                break;
            }
            case T_StaticCheckpoint:
            {
                txn_mgr_->create_static_checkpoint(context->log_mgr_);
                const char *message = "Static checkpoint created\n";
                memcpy(context->data_send_ + *(context->offset_), message, strlen(message));
                *(context->offset_) += static_cast<int>(strlen(message));
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;
        }

    } else if(auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
        switch (x->set_knob_type_)
        {
        case ast::SetKnobType::EnableNestLoop: {
            planner_->set_enable_nestedloop_join(x->bool_value_);
            break;
        }
        case ast::SetKnobType::EnableSortMerge: {
            planner_->set_enable_sortmerge_join(x->bool_value_);
            break;
        }
        case ast::SetKnobType::EnableHashJoin: {
            planner_->set_enable_hash_join(x->bool_value_);
            break;
        }
        default: {
            throw RMDBError("Not implemented!\n");
            break;
        }
        }
    } else if(auto x = std::dynamic_pointer_cast<LoadPlan>(plan)) {
        // Load CSV data from file into table.
        // The first line of every CSV is unconditionally treated as a header row
        // and discarded — files produced by the TPC-C data generator always carry
        // a header, which is the only supported input format.
        auto &tab = sm_manager_->db_.get_table(x->tab_name_);
        auto fh = sm_manager_->fhs_.at(x->tab_name_).get();

        std::ifstream csv_file(x->file_name_);
        if (!csv_file.is_open()) {
            throw RMDBError("Cannot open file: " + x->file_name_);
        }

        std::string line;
        if (!std::getline(csv_file, line)) {
            throw RMDBError("File is empty: " + x->file_name_);
        }

        int row_count = 0;
        while (std::getline(csv_file, line)) {
            if (line.empty()) {
                continue;
            }

            // Split line on commas.  This is a minimal splitter that does not
            // handle quoted fields, embedded commas, or embedded newlines —
            // sufficient for the narrow character set produced by the TPC-C
            // data generator, but not a general-purpose CSV parser.
            std::vector<std::string> tokens;
            std::string token;
            for (char ch : line) {
                if (ch == ',') {
                    tokens.push_back(std::move(token));
                    token.clear();
                } else {
                    token += ch;
                }
            }
            tokens.push_back(std::move(token));

            if (tokens.size() != tab.cols.size()) {
                throw RMDBError("Column count mismatch in file " + x->file_name_ +
                                ": expected " + std::to_string(tab.cols.size()) +
                                ", got " + std::to_string(tokens.size()));
            }

            // Build values from tokens, typed by column metadata.
            // std::stoi / std::stof throw std::invalid_argument or
            // std::out_of_range on malformed input, which are not project
            // exception types.  Convert them to RMDBError so the outer
            // handler in rmdb.cpp can deliver a diagnostic to the client.
            std::vector<Value> values;
            values.reserve(tokens.size());
            for (size_t i = 0; i < tokens.size(); i++) {
                Value val;
                try {
                    switch (tab.cols[i].type) {
                        case TYPE_INT: {
                            size_t parsed_len = 0;
                            int parsed_value = std::stoi(tokens[i], &parsed_len);
                            if (parsed_len != tokens[i].size()) {
                                throw std::invalid_argument("trailing characters");
                            }
                            val.set_int(parsed_value);
                            break;
                        }
                        case TYPE_FLOAT: {
                            size_t parsed_len = 0;
                            float parsed_value = std::stof(tokens[i], &parsed_len);
                            if (parsed_len != tokens[i].size()) {
                                throw std::invalid_argument("trailing characters");
                            }
                            val.set_float(parsed_value);
                            break;
                        }
                        case TYPE_STRING:
                            val.set_str(tokens[i]);
                            break;
                        case TYPE_DATETIME:
                            val = parse_datetime_literal(tokens[i]);
                            break;
                        default:
                            throw InternalError("Unexpected column type in LOAD");
                    }
                } catch (const std::invalid_argument &) {
                    throw RMDBError("Invalid value in file " + x->file_name_ +
                                    " row " + std::to_string(row_count + 1) +
                                    " col " + std::to_string(i + 1) +
                                    ": '" + tokens[i] + "'");
                } catch (const std::out_of_range &) {
                    throw RMDBError("Value out of range in file " + x->file_name_ +
                                    " row " + std::to_string(row_count + 1) +
                                    " col " + std::to_string(i + 1) +
                                    ": '" + tokens[i] + "'");
                }
                values.push_back(std::move(val));
            }

            // Insert a single row.  The pattern mirrors InsertExecutor::NextImpl
            // except that the record lock is acquired inside the try block so
            // that a TransactionAbortException from lock acquisition still
            // triggers release_reserved_rid — otherwise the slot is permanently
            // lost until the next process restart.
            {
                auto write_guard = context != nullptr && context->txn_mgr_ != nullptr
                                       ? context->txn_mgr_->write_txn_guard()
                                       : TransactionManager::WriteTxnGuard(nullptr);

                RmRecord rec(fh->get_file_hdr().record_size);
                for (size_t i = 0; i < values.size(); i++) {
                    auto &col = tab.cols[i];
                    Value val = coerce_value_to_type(values[i], col.type);
                    val.init_raw(col.len);
                    memcpy(rec.data + col.offset, val.raw->data, col.len);
                }

                Rid rid = fh->next_insert_rid();

                std::vector<std::pair<IndexMeta *, std::unique_ptr<char[]>>> inserted_keys;
                std::vector<std::string> violation_cols;
                try {
                    if (context != nullptr && context->lock_mgr_ != nullptr) {
                        context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, fh->GetFd());
                    }

                    for (size_t i = 0; i < tab.indexes.size(); ++i) {
                        auto &index = tab.indexes[i];
                        auto ih = sm_manager_->get_ih(x->tab_name_, index.cols);
                        auto key = std::make_unique<char[]>(index.col_tot_len);
                        index.build_key(key.get(), rec.data);
                        try {
                            ih->insert_entry(key.get(), rid, context->txn_);
                        } catch (const UniqueKeyViolationError &) {
                            violation_cols = index.col_names();
                            throw;
                        }
                        inserted_keys.emplace_back(&index, std::move(key));
                    }

                    lsn_t op_prev_lsn = context != nullptr && context->txn_ != nullptr
                                            ? context->txn_->get_prev_lsn()
                                            : INVALID_LSN;
                    lsn_t op_lsn = INVALID_LSN;
                    if (context != nullptr && context->txn_ != nullptr && context->log_mgr_ != nullptr) {
                        InsertLogRecord log_record(context->txn_->get_transaction_id(), rec, rid, x->tab_name_);
                        log_record.prev_lsn_ = op_prev_lsn;
                        op_lsn = context->log_mgr_->add_log_to_buffer(&log_record);
                        context->txn_->set_prev_lsn(op_lsn);
                    }

                    fh->insert_record(rid, rec.data, op_lsn);
                    if (context != nullptr && context->txn_ != nullptr) {
                        context->txn_->append_write_record(
                            new WriteRecord(WType::INSERT_TUPLE, x->tab_name_, rid, op_prev_lsn));
                    }
                } catch (const UniqueKeyViolationError &) {
                    for (auto it = inserted_keys.rbegin(); it != inserted_keys.rend(); ++it) {
                        auto ih = sm_manager_->get_ih(x->tab_name_, it->first->cols);
                        ih->delete_entry(it->second.get(), rid, context->txn_);
                    }
                    fh->release_reserved_rid(rid);
                    throw UniqueViolationError(x->tab_name_, violation_cols);
                } catch (...) {
                    for (auto it = inserted_keys.rbegin(); it != inserted_keys.rend(); ++it) {
                        auto ih = sm_manager_->get_ih(x->tab_name_, it->first->cols);
                        ih->delete_entry(it->second.get(), rid, context->txn_);
                    }
                    fh->release_reserved_rid(rid);
                    throw;
                }
            }

            row_count++;
        }

        write_bounded_output("Loaded " + std::to_string(row_count) + " rows into " + x->tab_name_ + "\n",
                             context);

    } else if(auto x = std::dynamic_pointer_cast<SetIsolationLevelPlan>(plan)) {
        context->isolation_level_ = x->level_;
    }
}

// 执行select语句，select语句的输出除了需要返回客户端外，还需要写入output.txt文件中
void QlManager::select_from(std::unique_ptr<AbstractExecutor> executorTreeRoot, std::vector<TabCol> sel_cols,
                            std::vector<std::string> output_names, Context *context) {
    std::vector<std::string> captions = std::move(output_names);
    if (captions.empty()) {
        captions.reserve(sel_cols.size());
        for (const auto &sel_col : sel_cols) {
            captions.push_back(sel_col.col_name);
        }
    }

    std::vector<size_t> col_widths;
    for (const auto &col : executorTreeRoot->cols()) {
        col_widths.push_back(col.type == TYPE_DATETIME ? RecordPrinter::DATETIME_COL_WIDTH
                                                       : RecordPrinter::DEFAULT_COL_WIDTH);
    }

    // Print header into buffer
    RecordPrinter rec_printer(std::move(col_widths));
    rec_printer.print_separator(context);
    rec_printer.print_record(captions, context);
    rec_printer.print_separator(context);
    // print header into file (collect in memory, write once after query)
    std::stringstream file_output;
    file_output << "|";
    for (size_t i = 0; i < captions.size(); ++i) {
        file_output << " " << captions[i] << " |";
    }
    file_output << "\n";

    // Print records
    size_t num_rec = 0;

    // 执行query_plan
   auto value_to_string = [](const Value &value) {
        if (value.type == TYPE_INT) {
            return std::to_string(value.int_val);
        }
        if (value.type == TYPE_FLOAT) {
            return std::to_string(value.float_val);
        }
        if (value.type == TYPE_STRING) {
            return value.str_val;
        }
        if (value.type == TYPE_DATETIME) {
            return format_datetime_value(value.datetime_val);
        }
        throw InternalError("Unexpected value type in select_from");
    };

    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        auto Tuple = executorTreeRoot->Next();
        std::vector<std::string> columns;
        for (auto &col : executorTreeRoot->cols()) {
            columns.push_back(value_to_string(get_col_value(*Tuple, col)));
        }
        rec_printer.print_record(columns, context);
        file_output << "|";
        for (size_t i = 0; i < columns.size(); ++i) {
            file_output << " " << columns[i] << " |";
        }
        file_output << "\n";
        num_rec++;
    }
    // Write all file output in one operation after query completes
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << file_output.str();
    outfile.close();
    // Print footer into buffer
    rec_printer.print_separator(context);
    // Print record count into buffer
    RecordPrinter::print_record_count(num_rec, context);
}

void QlManager::explain_analyze(std::unique_ptr<AbstractExecutor> executorTreeRoot, Context *context) {
    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        executorTreeRoot->Next();
    }

    std::string output;
    if (executorTreeRoot->explain_name().empty()) {
        output = "Project(rows=" + std::to_string(executorTreeRoot->rows()) + ")\n";
    } else {
        append_executor_tree(executorTreeRoot.get(), 0, output);
    }
    write_bounded_output(output, context);
}

// 执行DML语句
void QlManager::run_dml(std::unique_ptr<AbstractExecutor> exec){
    exec->Next();
}
