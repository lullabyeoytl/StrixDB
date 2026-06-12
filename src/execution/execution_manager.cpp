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
#include <iomanip>
#include <sstream>

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

auto render_file_table_output(const std::vector<std::string> &captions,
                              const std::vector<std::vector<std::string>> &rows,
                              const std::vector<size_t> &col_widths) -> std::string {
    std::stringstream output;
    auto append_separator = [&]() {
        for (size_t width : col_widths) {
            output << '+' << std::string(width + 2, '-');
        }
        output << "+\n";
    };
    auto append_record = [&](const std::vector<std::string> &record) {
        for (size_t i = 0; i < record.size(); ++i) {
            auto cell = record[i];
            size_t width = col_widths[i];
            if (cell.size() > width) {
                cell = cell.substr(0, width - 3) + "...";
            }
            output << "| " << std::setw(static_cast<int>(width)) << cell << ' ';
        }
        output << "|\n";
    };

    append_separator();
    append_record(captions);
    append_separator();
    for (const auto &row : rows) {
        append_record(row);
    }
    append_separator();
    output << "Total record(s): " << rows.size() << '\n';
    return output.str();
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
                // Apply the session-configured isolation level before any real work.
                context->txn_->set_isolation_level(context->isolation_level_);
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

    RecordPrinter rec_printer(col_widths);
    std::vector<std::vector<std::string>> rows;

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
        rows.push_back(std::move(columns));
    }

    const auto num_rec = rows.size();

    // Print result into buffer
    rec_printer.print_separator(context);
    rec_printer.print_record(captions, context);
    rec_printer.print_separator(context);
    for (const auto &row : rows) {
        rec_printer.print_record(row, context);
    }
    rec_printer.print_separator(context);
    RecordPrinter::print_record_count(num_rec, context);

    const auto file_output = render_file_table_output(captions, rows, col_widths);

    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << file_output;
    outfile.close();
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
