/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>

#include <fstream>

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

namespace {

auto format_index_columns(const IndexMeta &index) -> std::string {
    std::string cols = "(";
    for (size_t i = 0; i < index.cols.size(); ++i) {
        if (i != 0) {
            cols += ",";
        }
        cols += index.cols[i].name;
    }
    cols += ")";
    return cols;
}

auto format_index_unique(bool unique) -> std::string {
    return unique ? "unique" : "non_unique";
}

auto format_show_index_record(const std::string &tab_name, const IndexMeta &index) -> std::string {
    return "| " + tab_name + " | " + format_index_unique(index.unique) + " | " + format_index_columns(index) + " |";
}

/**
 * @brief: help class to flush all open files
 */
class StorageFlushService {
   public:
    explicit StorageFlushService(BufferPoolManager *buffer_pool_manager)
        : buffer_pool_manager_(buffer_pool_manager) {}

    bool Flush(const SmManager::StorageFiles &files) {
        for (auto *fh : files.record_files) {
            if (fh == nullptr) {
                continue;
            }
            fh->flush_file_header();
            if (!buffer_pool_manager_->flush_all_pages(fh->GetFd())) {
                return false;
            }
        }
        for (auto *ih : files.index_files) {
            if (ih == nullptr) {
                continue;
            }
            ih->flush_file_header();
            if (!buffer_pool_manager_->flush_all_pages(ih->get_index_fd())) {
                return false;
            }
        }
        return true;
    }

   private:
    BufferPoolManager *buffer_pool_manager_;
};

}  // namespace

void SmManager::set_txn_mgr(TransactionManager *txn_mgr) {
    txn_mgr_ = txn_mgr;
    for (auto &entry : db_.tabs_) {
        auto fh_it = fhs_.find(entry.first);
        if (fh_it == fhs_.end()) {
            continue;
        }
        int table_fd = fh_it->second->GetFd();
        for (auto &index : entry.second.indexes) {
            std::string ix_name = ix_manager_->get_index_name(entry.first, index.cols);
            auto ih_it = ihs_.find(ix_name);
            if (ih_it != ihs_.end()) {
                ih_it->second->set_table_fd(table_fd);
                ih_it->second->set_table_handle(fh_it->second.get());
                ih_it->second->set_index_cols(index.cols);
                ih_it->second->set_txn_mgr(txn_mgr_);
            }
        }
    }
}

std::string SmManager::fd_to_table_name(int fd) const {
    for (const auto &entry : fhs_) {
        if (entry.second != nullptr && entry.second->GetFd() == fd) {
            return entry.first;
        }
    }
    return {};
}

SmManager::StorageFiles SmManager::list_storage_files() const {
    StorageFiles files;
    files.record_files.reserve(fhs_.size());
    for (const auto &entry : fhs_) {
        files.record_files.push_back(entry.second.get());
    }
    files.index_files.reserve(ihs_.size());
    for (const auto &entry : ihs_) {
        files.index_files.push_back(entry.second.get());
    }
    return files;
}

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    if (mkdir(db_name.c_str(), 0755) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    try {
        std::filesystem::remove_all(db_name);
    } catch (...) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }
    std::ifstream ifs(DB_META_NAME);
    if (!ifs.is_open()) {
        throw DatabaseMetaCorruptedError(db_name);
    }
    db_ = DbMeta();
    ifs >> db_;
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();
    for (auto &entry : db_.tabs_) {
        fhs_.emplace(entry.first, rm_manager_->open_file(entry.first));
        // Open all index files for this table
        for (auto &index : entry.second.indexes) {
            std::string ix_name = ix_manager_->get_index_name(entry.first, index.cols);
            auto ih = ix_manager_->open_index(entry.first, index.cols);
            ih->set_table_fd(fhs_.at(entry.first)->GetFd());
            ih->set_table_handle(fhs_.at(entry.first).get());
            ih->set_index_cols(index.cols);
            ih->set_txn_mgr(txn_mgr_);
            ihs_.emplace(ix_name, std::move(ih));
        }
    }
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: flush all open files
 */
bool SmManager::flush_storage() {
    flush_meta();
    StorageFlushService flush_service(buffer_pool_manager_);
    return flush_service.Flush(list_storage_files());
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    flush_meta();
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();
    db_ = DbMeta();
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

void SmManager::show_index(const std::string& tab_name, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);

    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);

    for (auto &index : tab.indexes) {
        auto record = format_show_index_record(tab_name, index) + "\n";
        if (!context->ellipsis_ && *context->offset_ + RECORD_COUNT_LENGTH + record.length() < BUFFER_LENGTH) {
            memcpy(context->data_send_ + *context->offset_, record.c_str(), record.length());
            *context->offset_ += record.length();
        } else {
            context->ellipsis_ = true;
        }
        outfile << record;
    }

    outfile.close();
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context 
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    // Drop all indexes on the table first
    while (!tab.indexes.empty()) {
        std::vector<ColMeta> index_cols = tab.indexes.back().cols;
        drop_index(tab_name, index_cols, context);
    }

    // Close and remove record file handle
    auto fh_it = fhs_.find(tab_name);
    if (fh_it != fhs_.end()) {
        rm_manager_->close_file(fh_it->second.get());
        fhs_.erase(fh_it);
    }

    // Destroy record file
    rm_manager_->destroy_file(tab_name);

    // Remove from metadata
    db_.tabs_.erase(tab_name);

    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    // Get column metadata for each column name
    std::vector<ColMeta> index_cols;
    for (auto &col_name : col_names) {
        auto col_it = tab.get_col(col_name);
        index_cols.push_back(*col_it);
    }

    ix_manager_->create_index(tab_name, index_cols, true);

    std::string ix_name = ix_manager_->get_index_name(tab_name, index_cols);
    auto ih = ix_manager_->open_index(tab_name, index_cols);
    ih->set_table_fd(fhs_.at(tab_name)->GetFd());
    ih->set_table_handle(fhs_.at(tab_name).get());
    ih->set_index_cols(index_cols);
    ih->set_txn_mgr(context != nullptr && context->txn_mgr_ != nullptr ? context->txn_mgr_ : txn_mgr_);

    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.col_tot_len = 0;
    for (auto &col : index_cols) {
        index_meta.col_tot_len += col.len;
        index_meta.cols.push_back(col);
    }
    index_meta.col_num = index_cols.size();
    index_meta.unique = true;

    try {
        RmScan scan(fhs_.at(tab_name).get());
        while (!scan.is_end()) {
            Rid rid = scan.rid();
            auto rec = fhs_.at(tab_name)->get_record(rid, context);

            auto key = std::make_unique<char[]>(index_meta.col_tot_len);
            index_meta.build_key(key.get(), rec->data);
            ih->insert_entry(key.get(), rid, context->txn_);

            scan.next();
        }
    } catch (const UniqueKeyViolationError &) {
        ix_manager_->close_index(ih.get());
        ix_manager_->destroy_index(tab_name, index_cols);
        throw UniqueViolationError(tab_name, col_names);
    } catch (...) {
        ix_manager_->close_index(ih.get());
        ix_manager_->destroy_index(tab_name, index_cols);
        throw;
    }

    tab.indexes.push_back(index_meta);

    for (auto &col : tab.cols) {
        for (auto &index_col : index_cols) {
            if (col.name == index_col.name) {
                col.index = true;
            }
        }
    }

    ihs_.emplace(ix_name, std::move(ih));

    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    if (!tab.is_index(col_names)) {
        throw IndexNotFoundError(tab_name, col_names);
    }

    auto index_it = tab.get_index_meta(col_names);

    // Close and remove from ihs_ if open
    std::string ix_name = ix_manager_->get_index_name(tab_name, col_names);
    auto ih_it = ihs_.find(ix_name);
    if (ih_it != ihs_.end()) {
        ix_manager_->close_index(ih_it->second.get());
        ihs_.erase(ih_it);
    }

    // Destroy the index file on disk
    ix_manager_->destroy_index(tab_name, col_names);

    // Save column names before erasing the index
    std::vector<std::string> drop_col_names = index_it->col_names();

    // Remove index from table metadata
    tab.indexes.erase(index_it);

    // Update column index flag — only clear if column not in any remaining index
    for (auto &col : tab.cols) {
        for (auto &col_name : drop_col_names) {
            if (col.name == col_name) {
                bool still_indexed = false;
                for (auto &remaining : tab.indexes) {
                    for (auto &rc : remaining.cols) {
                        if (rc.name == col.name) {
                            still_indexed = true;
                            break;
                        }
                    }
                    if (still_indexed) break;
                }
                col.index = still_indexed;
            }
        }
    }

    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string> col_names;
    for (auto &col : cols) {
        col_names.push_back(col.name);
    }
    drop_index(tab_name, col_names, context);
}
