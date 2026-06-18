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
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include <cstdint>

#include "ix_defs.h"
#include "transaction/transaction.h"
#include "common/noncopyable.h"
#include "system/sm_meta.h"

class TransactionManager;
class RmFileHandle;

enum class Operation { FIND = 0, INSERT, DELETE };  // 三种操作：查找、插入、删除
enum class LatchMode { SHARED, EXCLUSIVE };
enum class InsertEntryOptions { ValidateUnique = 0, SkipUniqueProbe };

struct IxBulkLoadEntry {
    const char *key;
    Rid rid;
};

static const bool binary_search = false;

struct ScanUpperBound {
    bool has_bound = false;
    std::vector<char> key;
    bool inclusive = false;
};

inline int ix_compare(const char *a, const char *b, ColType type, int col_len) {
    switch (type) {
        case TYPE_INT: {
            int ia = *(int *)a;
            int ib = *(int *)b;
            return three_way_compare(ia, ib);
        }
        case TYPE_FLOAT: {
            float fa = *(float *)a;
            float fb = *(float *)b;
            return three_way_compare(fa, fb);
        }
        case TYPE_STRING:
            return memcmp(a, b, col_len);
        case TYPE_DATETIME: {
            int64_t da = *reinterpret_cast<const int64_t *>(a);
            int64_t db = *reinterpret_cast<const int64_t *>(b);
            return three_way_compare(da, db);
        }
        default:
            throw InternalError("Unexpected data type");
    }
}

inline int ix_compare(const char* a, const char* b, const std::vector<ColType>& col_types, const std::vector<int>& col_lens) {
    int offset = 0;
    for(size_t i = 0; i < col_types.size(); ++i) {
        int res = ix_compare(a + offset, b + offset, col_types[i], col_lens[i]);
        if(res != 0) return res;
        offset += col_lens[i];
    }
    return 0;
}

/* 管理B+树中的每个节点 */
class IxNodeHandle {
    friend class IxIndexHandle;
    friend class IxScan;

   private:
    const IxFileHdr *file_hdr;      // 节点所在文件的头部信息
    Page *page;                     // 存储节点的页面
    IxPageHdr *page_hdr;            // page->data的第一部分，指针指向首地址，长度为sizeof(IxPageHdr)
    char *keys;                     // page->data的第二部分，指针指向首地址，长度为file_hdr->keys_size_，每个key的长度为file_hdr->col_tot_len_
    Rid *rids;                      // page->data的第三部分，指针指向首地址

   public:
    IxNodeHandle() = default;

    IxNodeHandle(const IxFileHdr *file_hdr_, Page *page_) : file_hdr(file_hdr_), page(page_) {
        page_hdr = reinterpret_cast<IxPageHdr *>(page->get_data());
        keys = page->get_data() + sizeof(IxPageHdr);
        rids = reinterpret_cast<Rid *>(keys + file_hdr->keys_size_);
    }

    int get_size() const { return page_hdr->num_key; }

    void set_size(int size) { page_hdr->num_key = size; }

    int get_max_size() const { return file_hdr->btree_order_ + 1; }

    int get_min_size() const { return get_max_size() / 2; }

    int key_at(int i) const { return *(int *)get_key(i); }

    /* 得到第i个孩子结点的page_no */
    page_id_t value_at(int i) const { return get_rid(i)->page_no; }

    page_id_t get_page_no() const { return page->get_page_id().page_no; }

    PageId get_page_id() const { return page->get_page_id(); }

    void RLatch() { page->RLatch(); }

    void RUnlatch() { page->RUnlatch(); }

    void WLatch() { page->WLatch(); }

    void WUnlatch() { page->WUnlatch(); }

    page_id_t get_next() const { return page_hdr->next; }

    page_id_t get_prev() const { return page_hdr->prev; }

    page_id_t get_parent() const { return page_hdr->parent; }

    bool is_leaf_page() const { return page_hdr->is_leaf; }

    bool is_root_page() const { return get_parent() == INVALID_PAGE_ID; }

    bool is_split_incomplete() const { return page_hdr->split_state == IxPageSplitState::SPLIT_INCOMPLETE; }

    bool is_live() const { return page_hdr->recycle_state == IxPageRecycleState::LIVE; }

    bool is_half_dead() const { return page_hdr->recycle_state == IxPageRecycleState::HALF_DEAD; }

    bool is_deleted() const { return page_hdr->recycle_state == IxPageRecycleState::DELETED; }

    bool is_reusable() const { return page_hdr->recycle_state == IxPageRecycleState::REUSABLE; }

    void set_next(page_id_t page_no) { page_hdr->next = page_no; }

    void set_prev(page_id_t page_no) { page_hdr->prev = page_no; }

    void set_parent(page_id_t parent) { page_hdr->parent = parent; }

    void mark_split_incomplete() { page_hdr->split_state = IxPageSplitState::SPLIT_INCOMPLETE; }

    void clear_split_incomplete() { page_hdr->split_state = IxPageSplitState::NORMAL; }

    void mark_live() {
        page_hdr->recycle_state = IxPageRecycleState::LIVE;
        page_hdr->deleted_epoch = 0;
    }

    void mark_half_dead(page_id_t epoch) {
        page_hdr->recycle_state = IxPageRecycleState::HALF_DEAD;
        page_hdr->deleted_epoch = epoch;
    }

    void mark_deleted(page_id_t epoch) {
        page_hdr->recycle_state = IxPageRecycleState::DELETED;
        page_hdr->deleted_epoch = epoch;
    }

    void mark_reusable() { page_hdr->recycle_state = IxPageRecycleState::REUSABLE; }

    char *get_key(int key_idx) const { return keys + key_idx * file_hdr->col_tot_len_; }

    Rid *get_rid(int rid_idx) const { return &rids[rid_idx]; }

    void set_key(int key_idx, const char *key) { memcpy(keys + key_idx * file_hdr->col_tot_len_, key, file_hdr->col_tot_len_); }

    void set_rid(int rid_idx, const Rid &rid) { rids[rid_idx] = rid; }

    char *get_high_key() const { return get_key(file_hdr->btree_order_); }

    void set_high_key(const char *key) { memcpy(get_high_key(), key, file_hdr->col_tot_len_); }

    // Internal pages never use IX_LEAF_HEADER_PAGE as a right link.
    bool has_next() const { return get_next() != IX_NO_PAGE && get_next() != IX_LEAF_HEADER_PAGE; }

    bool is_safe_for_insert() const { return get_size() < get_max_size() - 1; }

    bool is_safe_for_delete() const { return get_size() > get_min_size(); }

    int lower_bound(const char *target) const;

    int upper_bound(const char *target) const;

    void insert_pairs(int pos, const char *key, const Rid *rid, int n);

    page_id_t internal_lookup_ub(const char *key);
    
    page_id_t internal_lookup_lb(const char *key);

    bool leaf_lookup(const char *key, Rid **value);

    int insert(const char *key, const Rid &value);

    // 用于在结点中的指定位置插入单个键值对
    void insert_pair(int pos, const char *key, const Rid &rid) { insert_pairs(pos, key, &rid, 1); }

    void erase_pair(int pos);

    int remove(const char *key);

    // 精确删除：按 (key, rid) 匹配并删除，返回是否找到
    bool remove(const char *key, const Rid &rid);

    /**
     * @brief used in internal node to remove the last key in root node, and return the last child
     *
     * @return the last child
     */
    page_id_t remove_and_return_only_child() {
        if (get_size() != 1) {
            throw InternalError("remove_and_return_only_child: expected size 1");
        }
        page_id_t child_page_no = value_at(0);
        erase_pair(0);
        if (get_size() != 0) {
            throw InternalError("remove_and_return_only_child: size not 0 after erase");
        }
        return child_page_no;
    }

    /**
     * @brief 由parent调用，寻找child，返回child在parent中的rid_idx∈[0,page_hdr->num_key)
     * @param child
     * @return int
     */
    int find_child(IxNodeHandle *child) {
        int rid_idx;
        for (rid_idx = 0; rid_idx < page_hdr->num_key; rid_idx++) {
            if (get_rid(rid_idx)->page_no == child->get_page_no()) {
                break;
            }
        }
        if (rid_idx >= page_hdr->num_key) {
            throw InternalError("find_child: child not found");
        }
        return rid_idx;
    }
};

/* B+树 */
class IxIndexHandle : public NonCopyable {
    friend class IxScan;
    friend class IxManager;

   private:
   DiskManager *disk_manager_;
   BufferPoolManager *buffer_pool_manager_;
   int fd_;                                    // 存储B+树的文件
   int table_fd_ = -1;
   RmFileHandle *table_handle_ = nullptr;
   std::vector<ColMeta> index_cols_;
   TransactionManager *txn_mgr_ = nullptr;
   IxFileHdr* file_hdr_;                       // 存了root_page，但其初始化为2（第0页存FILE_HDR_PAGE，第1页存LEAF_HEADER_PAGE）
   mutable std::mutex file_hdr_latch_;
   mutable std::mutex create_node_latch_;
   mutable std::mutex cleanup_latch_;
   mutable std::mutex unique_insert_latch_;
   page_id_t cleanup_epoch_ = 0;
   mutable std::atomic<int> active_accessors_{0};

   public:
    IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd);
    ~IxIndexHandle();

    void set_table_fd(int table_fd) { table_fd_ = table_fd; }

    int get_table_fd() const { return table_fd_; }

    void set_table_handle(RmFileHandle *table_handle) { table_handle_ = table_handle; }

    void set_index_cols(std::vector<ColMeta> cols) { index_cols_ = std::move(cols); }

    int get_index_fd() const { return fd_; }

    void flush_file_header() const {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        std::vector<char> data(file_hdr_->tot_len_);
        file_hdr_->serialize(data.data());
        disk_manager_->write_page(fd_, IX_FILE_HDR_PAGE, data.data(), file_hdr_->tot_len_);
    }

    void set_txn_mgr(TransactionManager *txn_mgr) { txn_mgr_ = txn_mgr; }

    TransactionManager *get_txn_mgr() const { return txn_mgr_; }

    bool validate_unique_key(const char *key, Transaction *transaction, const Rid *self_rid = nullptr,
                             const std::vector<Rid> *ignored_rids = nullptr);

    // for search
    bool get_value(const char *key, std::vector<Rid> *result, Transaction *transaction);

    std::unique_ptr<IxNodeHandle> find_leaf_page(const char *key, Operation operation, Transaction *transaction,
                                                 bool find_first = false);

    // for insert
    page_id_t insert_entry(const char *key, const Rid &value, Transaction *transaction,
                           const std::vector<Rid> *ignored_rids = nullptr,
                           InsertEntryOptions options = InsertEntryOptions::ValidateUnique);

    bool bulk_load_sorted_entries(const std::vector<IxBulkLoadEntry> &entries, Transaction *transaction);

    std::unique_ptr<IxNodeHandle> split(IxNodeHandle *node);

    void insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node, Transaction *transaction);

    // for delete
    bool delete_entry(const char *key, const Rid &rid, Transaction *transaction);

    Iid lower_bound(const char *key);

    Iid upper_bound(const char *key);

    Iid leaf_end() const;

    Iid leaf_begin() const;

    bool has_duplicate_keys() const;

    int cleanup_empty_leaf_pages();

   private:
    class AccessGuard : public NonCopyable {
       public:
        explicit AccessGuard(const IxIndexHandle *ih) : ih_(ih) { ih_->active_accessors_.fetch_add(1); }
        ~AccessGuard() { ih_->active_accessors_.fetch_sub(1); }

       private:
        const IxIndexHandle *ih_;
    };

    AccessGuard guard_access() const { return AccessGuard(this); }

    // 辅助函数
    void update_root_page_no(page_id_t root) {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        file_hdr_->root_page_ = root;
    }

    void update_tree_page_nos(page_id_t root, page_id_t first_leaf, page_id_t last_leaf) {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        file_hdr_->root_page_ = root;
        file_hdr_->first_leaf_ = first_leaf;
        file_hdr_->last_leaf_ = last_leaf;
    }

    page_id_t get_root_page_no() const {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        return file_hdr_->root_page_;
    }

    page_id_t get_first_leaf_page_no() const {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        return file_hdr_->first_leaf_;
    }

    page_id_t get_last_leaf_page_no() const {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        return file_hdr_->last_leaf_;
    }

    int get_num_pages() const {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        return file_hdr_->num_pages_;
    }

    void set_first_leaf_page_no(page_id_t page_no) {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        file_hdr_->first_leaf_ = page_no;
    }

    void set_last_leaf_page_no(page_id_t page_no) {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        file_hdr_->last_leaf_ = page_no;
    }

    void increment_num_pages() {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        file_hdr_->num_pages_++;
    }

    page_id_t advance_cleanup_epoch() {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        return ++cleanup_epoch_;
    }

    page_id_t pop_free_page_no() {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        page_id_t free_page_no = file_hdr_->first_free_page_no_;
        if (free_page_no != IX_NO_PAGE) {
            Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, free_page_no});
            if (page == nullptr) {
                throw BufferPoolExhaustedError();
            }
            IxNodeHandle free_node(file_hdr_, page);
            file_hdr_->first_free_page_no_ = free_node.page_hdr->next_free_page_no;
            buffer_pool_manager_->unpin_page(free_node.get_page_id(), false);
        }
        return free_page_no;
    }

    void push_free_page_no(page_id_t page_no) {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
        if (page == nullptr) {
            throw BufferPoolExhaustedError();
        }
        IxNodeHandle node(file_hdr_, page);
        node.page_hdr->next_free_page_no = file_hdr_->first_free_page_no_;
        node.mark_reusable();
        file_hdr_->first_free_page_no_ = page_no;
        buffer_pool_manager_->unpin_page(node.get_page_id(), true);
    }

    bool is_empty() const { return get_root_page_no() == IX_NO_PAGE; }

    // for get/create node
    std::unique_ptr<IxNodeHandle> fetch_node(int page_no) const;

    std::unique_ptr<IxNodeHandle> create_node();

    // for maintain data structure
    void maintain_parent(IxNodeHandle *node);

    void maintain_parent(page_id_t child_page_no, page_id_t parent_page_no, const char *child_first_key);

    struct VerifiedParent {
        std::unique_ptr<IxNodeHandle> parent;
        int child_idx;
    };

    VerifiedParent latch_parent_containing_child(page_id_t child_page_no, const char *separator_key,
                                                 page_id_t parent_hint);

    page_id_t locate_parent_page_from_root(page_id_t child_page_no, const char *separator_key);

    page_id_t locate_parent_page_from(page_id_t page_no, page_id_t child_page_no, const char *separator_key);

    int find_child_index(const IxNodeHandle *parent, page_id_t child_page_no) const;

    void erase_leaf(IxNodeHandle *leaf);

    bool can_delete_leaf(const IxNodeHandle *leaf) const;

    bool try_delete_empty_leaf(page_id_t leaf_page_no, page_id_t epoch);

    bool try_recycle_deleted_page(page_id_t page_no, page_id_t epoch);

    bool unlink_half_dead_leaf(IxNodeHandle *leaf, IxNodeHandle *parent, int parent_idx, page_id_t epoch);

    void release_node_handle(IxNodeHandle &node);

    void recycle_node_page(IxNodeHandle *node);

    void maintain_child(IxNodeHandle *node, int child_idx);

    bool is_ignored_unique_hit(const Rid &hit, const Rid *self_rid, const std::vector<Rid> *ignored_rids) const;

    void validate_unique_hit(const Rid &hit, const Rid *self_rid, const std::vector<Rid> *ignored_rids) const;

    void validate_unique_key_in_latched_leaf(const char *key, const IxNodeHandle *leaf, Transaction *transaction,
                                             const Rid *self_rid,
                                             const std::vector<Rid> *ignored_rids) const;

    // Walk backward from `start` to the first leaf whose last key is < `key`
    std::unique_ptr<IxNodeHandle> backtrack_leaf(std::unique_ptr<IxNodeHandle> start, const char *key,
                                                LatchMode latch_mode);
    
    // Returns true if `key` should be moved to the right child of `node`
    // 当并发分裂在右侧山城新节点，原节点high_key被提升，查询key可能右移
    bool should_move_right(const IxNodeHandle *node, const char *key) const;

    void unlatch_and_unpin_shared(std::unique_ptr<IxNodeHandle> &node) const;

    void unlatch_and_unpin_exclusive(std::unique_ptr<IxNodeHandle> &node, bool is_dirty) const;

    void move_right_with_shared_latch(std::unique_ptr<IxNodeHandle> &node) const;

    void move_right_with_exclusive_latch(std::unique_ptr<IxNodeHandle> &node) const;

    // for index test
    Rid get_rid(const Iid &iid) const;
};
