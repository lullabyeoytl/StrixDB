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

#include <memory>
#include <string>

#include "system/sm_meta.h"
#include "ix_defs.h"
#include "ix_index_handle.h"

/**
 * @brief 索引管理器，负责索引的创建、删除、打开和关闭等操作
 * 索引文件的命名规则为：表名_字段1_字段2_..._字段n.idx
 */
class IxManager {
   private:
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;

    std::unique_ptr<IxIndexHandle> open_index_on_fd(int fd) {
        try {
            return std::make_unique<IxIndexHandle>(disk_manager_, buffer_pool_manager_, fd);
        } catch (...) {
            disk_manager_->close_file(fd);
            throw;
        }
    }

   public:
    IxManager(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager)
        : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager) {}

    std::string get_index_name(const std::string &filename, const std::vector<std::string>& index_cols) {
        std::string index_name = filename;
        for(size_t i = 0; i < index_cols.size(); ++i) 
            index_name += "_" + index_cols[i];
        index_name += ".idx";

        return index_name;
    }

    std::string get_index_name(const std::string &filename, const std::vector<ColMeta>& index_cols) {
        std::string index_name = filename;
        for(size_t i = 0; i < index_cols.size(); ++i) 
            index_name += "_" + index_cols[i].name;
        index_name += ".idx";

        return index_name;
    }

    bool exists(const std::string &filename, const std::vector<ColMeta>& index_cols) {
        auto ix_name = get_index_name(filename, index_cols);
        return disk_manager_->is_file(ix_name);
    }

    bool exists(const std::string &filename, const std::vector<std::string>& index_cols) {
        auto ix_name = get_index_name(filename, index_cols);
        return disk_manager_->is_file(ix_name);
    }

    void create_index(const std::string &filename, const std::vector<ColMeta>& index_cols) {
        create_index(filename, index_cols, false);
    }

    void create_index(const std::string &filename, const std::vector<ColMeta>& index_cols, bool unique) {
        std::string ix_name = get_index_name(filename, index_cols);
        disk_manager_->create_file(ix_name);
        int fd = disk_manager_->open_file(ix_name);

        int col_tot_len = 0;
        int col_num = index_cols.size();
        for(auto& col: index_cols) {
            col_tot_len += col.len;
        }
        if (col_tot_len > IX_MAX_COL_LEN) {
            throw InvalidColLengthError(col_tot_len);
        }
        int btree_order = static_cast<int>((PAGE_SIZE - sizeof(IxPageHdr)) / (col_tot_len + sizeof(Rid)) - 1);
        assert(btree_order > 2);

        IxFileHdr* fhdr = new IxFileHdr(IX_NO_PAGE, IX_INIT_NUM_PAGES, IX_INIT_ROOT_PAGE,
                                col_num, col_tot_len, btree_order, (btree_order + 1) * col_tot_len,
                                IX_INIT_ROOT_PAGE, IX_INIT_ROOT_PAGE);
        for(int i = 0; i < col_num; ++i) {
            fhdr->col_types_.push_back(index_cols[i].type);
            fhdr->col_lens_.push_back(index_cols[i].len);
        }
        fhdr->set_unique(unique);
        fhdr->update_tot_len();

        char* data = new char[fhdr->tot_len_];
        fhdr->serialize(data);
        disk_manager_->write_page(fd, IX_FILE_HDR_PAGE, data, fhdr->tot_len_);

        char page_buf[PAGE_SIZE];
        memset(page_buf, 0, PAGE_SIZE);
        // leaf header page (page 1): leaf node, prev/next point to root node
        {
            memset(page_buf, 0, PAGE_SIZE);
            auto phdr = reinterpret_cast<IxPageHdr *>(page_buf);
            *phdr = {
                .next_free_page_no = IX_NO_PAGE,
                .parent = IX_NO_PAGE,
                .num_key = 0,
                .is_leaf = true,
                .prev = IX_INIT_ROOT_PAGE,
                .next = IX_INIT_ROOT_PAGE,
            };
            disk_manager_->write_page(fd, IX_LEAF_HEADER_PAGE, page_buf, PAGE_SIZE);
        }
        // root node (page 2): leaf node, prev/next point to leaf header
        {
            memset(page_buf, 0, PAGE_SIZE);
            auto phdr = reinterpret_cast<IxPageHdr *>(page_buf);
            *phdr = {
                .next_free_page_no = IX_NO_PAGE,
                .parent = IX_NO_PAGE,
                .num_key = 0,
                .is_leaf = true,
                .prev = IX_LEAF_HEADER_PAGE,
                .next = IX_LEAF_HEADER_PAGE,
            };
            disk_manager_->write_page(fd, IX_INIT_ROOT_PAGE, page_buf, PAGE_SIZE);
        }

        disk_manager_->set_fd2pageno(fd, IX_INIT_NUM_PAGES);
        disk_manager_->close_file(fd);
    }

    void destroy_index(const std::string &filename, const std::vector<ColMeta>& index_cols) {
        std::string ix_name = get_index_name(filename, index_cols);
        disk_manager_->destroy_file(ix_name);
    }

    void destroy_index(const std::string &filename, const std::vector<std::string>& index_cols) {
        std::string ix_name = get_index_name(filename, index_cols);
        disk_manager_->destroy_file(ix_name);
    }

    // 注意这里打开文件，创建并返回了index file handle的指针
    std::unique_ptr<IxIndexHandle> open_index(const std::string &filename, const std::vector<ColMeta>& index_cols) {
        std::string ix_name = get_index_name(filename, index_cols);
        int fd = disk_manager_->open_file(ix_name);
        return open_index_on_fd(fd);
    }

    std::unique_ptr<IxIndexHandle> open_index(const std::string &filename, const std::vector<std::string>& index_cols) {
        std::string ix_name = get_index_name(filename, index_cols);
        int fd = disk_manager_->open_file(ix_name);
        return open_index_on_fd(fd);
    }

    void close_index(const IxIndexHandle *ih) {
        char* data = new char[ih->file_hdr_->tot_len_];
        ih->file_hdr_->serialize(data);
        disk_manager_->write_page(ih->fd_, IX_FILE_HDR_PAGE, data, ih->file_hdr_->tot_len_);
        // 缓冲区的所有页刷到磁盘，注意这句话必须写在close_file前面
        buffer_pool_manager_->flush_all_pages(ih->fd_);
        disk_manager_->close_file(ih->fd_);
    }
};
