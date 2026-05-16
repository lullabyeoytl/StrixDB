/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include "ix_scan.h"

namespace {
constexpr int IX_SPLIT_PUBLISH_RETRY_LIMIT = 128;
constexpr int IX_PARENT_RELOCATION_RETRY_LIMIT = 32;
}

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    // 二分查找第一个 >= target 的 key，范围 [0, num_key)
    int lo = 0, hi = page_hdr->num_key;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，叶节点范围为[0,num_key)，内部节点范围为[1,num_key)。如果返回的key_idx=num_key，则表示target大于等于最后一个key
 */
int IxNodeHandle::upper_bound(const char *target) const {
    // 二分查找第一个 > target 的 key
    // 叶子节点: 范围 [0, num_key)；内部节点: 范围 [1, num_key) (keys[0] 不参与路由)
    int lo = page_hdr->is_leaf ? 0 : 1;
    int hi = page_hdr->num_key;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key &&
        ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        *value = get_rid(pos);
        return true;
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t IxNodeHandle::internal_lookup_ub(const char *key) {
    // upper_bound 搜索 [1, num_key)，返回第一个 > key 的路由 key 位置
    // 目标孩子节点在 value_at(pos - 1)
    int pos = upper_bound(key);
    return value_at(pos - 1);
}

page_id_t IxNodeHandle::internal_lookup_lb(const char *key) {
    // lower_bound 搜索 [0, num_key)，返回第一个 >= key 的路由 key 位置
    // 若命中分隔键，则目标孩子节点在 value_at(pos)
    // 否则应当落到其左侧孩子 value_at(pos - 1)
    int pos = lower_bound(key);
    if (pos == page_hdr->num_key) {
        return value_at(page_hdr->num_key - 1);
    }
    if (pos == 0) {
        return value_at(0);
    }
    if (ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        return value_at(pos);
    }
    return value_at(pos - 1);
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param (key, rid) 连续键值对的起始地址，也就是第一个键值对，可以通过(key, rid)来获取n个键值对
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    if (pos < 0 || pos > page_hdr->num_key) {
        throw InternalError("insert_pairs: pos out of range");
    }
    int ct_len = file_hdr->col_tot_len_;
    int move_cnt = page_hdr->num_key - pos;
    // 2. 右移已有的 keys 和 rids 腾出空间
    if (move_cnt > 0) {
        memmove(keys + (pos + n) * ct_len, keys + pos * ct_len, move_cnt * ct_len);
        memmove(rids + pos + n, rids + pos, move_cnt * sizeof(Rid));
    }
    // 3. 拷贝新的 keys 和 rids
    memcpy(keys + pos * ct_len, key, n * ct_len);
    memcpy(rids + pos, rid, n * sizeof(Rid));
    // 4. 更新键数量
    page_hdr->num_key += n;
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param (key, value) 要插入的键值对
 * @return int 键值对数量
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    insert_pair(pos, key, value);
    return page_hdr->num_key;
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void IxNodeHandle::erase_pair(int pos) {
    if (pos < 0 || pos >= page_hdr->num_key) {
        throw InternalError("erase_pair: pos out of range");
    }
    int ct_len = file_hdr->col_tot_len_;
    int move_cnt = page_hdr->num_key - 1 - pos;
    // 左移 keys 和 rids 覆盖被删除位置
    if (move_cnt > 0) {
        memmove(keys + pos * ct_len, keys + (pos + 1) * ct_len, move_cnt * ct_len);
        memmove(rids + pos, rids + pos + 1, move_cnt * sizeof(Rid));
    }
    // 更新键数量
    page_hdr->num_key--;
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key &&
        ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        erase_pair(pos);
    }
    return page_hdr->num_key;
}

bool IxNodeHandle::remove(const char *key, const Rid &rid) {
    int pos = lower_bound(key);
    while (pos < page_hdr->num_key &&
           ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        if (get_rid(pos)->page_no == rid.page_no && get_rid(pos)->slot_no == rid.slot_no) {
            erase_pair(pos);
            return true;
        }
        pos++;
    }
    return false;
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    std::vector<char> buf(PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf.data(), PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf.data());

    // disk_manager管理的fd对应的文件中，设置从file_hdr_->num_pages开始分配page_no
    disk_manager_->set_fd2pageno(fd, file_hdr_->num_pages_);
}

IxIndexHandle::~IxIndexHandle() {
    delete file_hdr_;
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @return [leaf node] and [root_is_latched] 返回目标叶子结点以及根结点是否加锁
 * @note need to Unlatch and unpin the leaf node outside!
 * 注意：用了FindLeafPage之后一定要unlatch叶结点，否则下次latch该结点会堵塞！
 */
std::unique_ptr<IxNodeHandle> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    page_id_t page_no = get_root_page_no();
    auto node = fetch_node(page_no);
    node->RLatch();

    while (true) {
        while (node->is_deleted() || (!node->is_leaf_page() && should_move_right(node.get(), key))) {
            move_right_with_shared_latch(node);
        }
        if (node->is_leaf_page()) {
            break;
        }

        page_id_t child_page_no = node->internal_lookup_ub(key);
        // Release the parent before waiting on the child; right links repair a stale route.
        unlatch_and_unpin_shared(node);
        auto child = fetch_node(child_page_no);
        child->RLatch();
        node = std::move(child);
    }

    while (node->is_deleted() || should_move_right(node.get(), key)) {
        move_right_with_shared_latch(node);
    }

    return node;
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 */
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    auto access_guard = guard_access();
    Iid start = lower_bound(key);
    ScanUpperBound upper_bound;
    upper_bound.has_bound = true;
    upper_bound.key.assign(key, key + file_hdr_->col_tot_len_);
    upper_bound.inclusive = true;
    IxScan scan(this, start, std::move(upper_bound), buffer_pool_manager_);
    while (!scan.is_end()) {
        result->push_back(scan.rid());
        scan.next();
    }
    return !result->empty();
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note need to unpin the new node outside
 * 注意：本函数执行完毕后，原node和new node都需要在函数外面进行unpin
 */
std::unique_ptr<IxNodeHandle> IxIndexHandle::split(IxNodeHandle *node) {
    // The new right sibling remains write-latched until its parent hint is installed.
    auto new_node = create_node();
    new_node->WLatch();
    new_node->page_hdr->is_leaf = node->is_leaf_page();

    int ct_len = file_hdr_->col_tot_len_;
    int total = node->get_size();
    int mid = total / 2;  // left: [0, mid); right: [mid, total)
    int right_cnt = total - mid;
    if (right_cnt <= 0) {
        throw InternalError("split: invalid right_cnt");
    }
    std::vector<char> old_high_key(ct_len);
    bool old_has_next = node->has_next();
    if (old_has_next) {
        memcpy(old_high_key.data(), node->get_high_key(), ct_len);
    }

    // Publish the right link before the parent downlink so readers can cross the split.
    memcpy(new_node->keys, node->keys + mid * ct_len, right_cnt * ct_len);
    memcpy(new_node->rids, node->rids + mid, right_cnt * sizeof(Rid));
    new_node->set_size(right_cnt);
    node->set_size(mid);
    new_node->set_next(node->get_next());
    node->set_next(new_node->get_page_no());
    node->set_high_key(new_node->get_key(0));
    // The left page owns this marker until its parent downlink is installed.
    node->mark_split_incomplete();
    // Preserve the original upper fence for the new sibling when one existed.
    if (new_node->has_next() && old_has_next) {
        new_node->set_high_key(old_high_key.data());
    }

    if (node->is_leaf_page()) {
        // Prev is a leaf-chain hint; the old right sibling is not waited on during split.
        new_node->set_prev(node->get_page_no());
        if (get_last_leaf_page_no() == node->get_page_no()) {
            set_last_leaf_page_no(new_node->get_page_no());
        }
    } else {
        // Internal child parent pointers are hints; relocation verifies by child page number.
        new_node->set_prev(IX_NO_PAGE);
    }
    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param (old_node, new_node) 原结点为old_node，old_node被分裂之后产生了新的右兄弟结点new_node
 * @param key 要插入parent的key
 * @note 一个结点插入了键值对之后需要分裂，分裂后左半部分的键值对保留在原结点，在参数中称为old_node，
 * 右半部分的键值对分裂为新的右兄弟节点，在参数中称为new_node（参考Split函数来理解old_node和new_node）
 * @note 本函数执行完毕后，new node和old node都需要在函数外面进行unpin
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    if (old_node->is_root_page()) {
        // Root publication installs both downlinks before clearing the old root split marker.
        auto new_root = create_node();
        new_root->WLatch();
        new_root->page_hdr->is_leaf = false;
        new_root->set_parent(IX_NO_PAGE);
        new_root->set_size(2);
        memcpy(new_root->get_key(0), old_node->get_key(0), file_hdr_->col_tot_len_);
        memcpy(new_root->get_key(1), key, file_hdr_->col_tot_len_);
        new_root->set_rid(0, Rid{old_node->get_page_no(), 0});
        new_root->set_rid(1, Rid{new_node->get_page_no(), 0});
        old_node->set_parent(new_root->get_page_no());
        new_node->set_parent(new_root->get_page_no());
        update_root_page_no(new_root->get_page_no());
        old_node->clear_split_incomplete();
        new_node->WUnlatch();
        new_root->WUnlatch();
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        return;
    }

    page_id_t old_page_no = old_node->get_page_no();
    page_id_t parent_hint = old_node->get_parent();

    // Parent hints can be stale; the actual parent must contain the left child downlink.
    auto verified_parent = latch_parent_containing_child(old_page_no, key, parent_hint);
    auto &parent = verified_parent.parent;
    int idx = verified_parent.child_idx;

    parent->insert_pair(idx + 1, key, Rid{new_node->get_page_no(), 0});
    // The right sibling stays write-latched until its parent hint matches the installed downlink.
    new_node->set_parent(parent->get_page_no());
    new_node->WUnlatch();

    if (parent->get_size() >= parent->get_max_size()) {
        // Parent overflow follows the same incomplete-split protocol recursively.
        auto new_parent = split(parent.get());
        std::vector<char> mid_key(file_hdr_->col_tot_len_);
        memcpy(mid_key.data(), new_parent->get_key(0), file_hdr_->col_tot_len_);
        insert_into_parent(parent.get(), mid_key.data(), new_parent.get(), transaction);
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
    }

    old_node->clear_split_incomplete();
    unlatch_and_unpin_exclusive(parent, true);
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param (key, value) 要插入的键值对
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    auto access_guard = guard_access();
    std::unique_ptr<IxNodeHandle> leaf;
    int split_publish_waits = 0;
    while (true) {
        leaf = find_leaf_page(key, Operation::INSERT, transaction);
        leaf->RUnlatch();
        leaf->WLatch();

        while (leaf->is_deleted() || should_move_right(leaf.get(), key)) {
            move_right_with_exclusive_latch(leaf);
        }

        if (leaf->is_split_incomplete() && leaf->get_size() + 1 >= leaf->get_max_size()) {
            // A second split on the same left page waits for the pending parent publish.
            unlatch_and_unpin_exclusive(leaf, false);
            if (++split_publish_waits >= IX_SPLIT_PUBLISH_RETRY_LIMIT) {
                throw InternalError("B-link split publication retry limit reached");
            }
            std::this_thread::yield();
            continue;
        }
        break;
    }

    if (file_hdr_->unique_) {
        Rid *found = nullptr;
        if (leaf->leaf_lookup(key, &found)) {
            unlatch_and_unpin_exclusive(leaf, false);
            throw UniqueKeyViolationError();
        }
    }

    bool first_key_may_change =
        leaf->get_size() == 0 || ix_compare(key, leaf->get_key(0), file_hdr_->col_types_, file_hdr_->col_lens_) <= 0;
    page_id_t leaf_page_no = leaf->get_page_no();
    page_id_t leaf_parent_page_no = leaf->get_parent();
    std::vector<char> updated_first_key(file_hdr_->col_tot_len_);
    bool should_update_parent = false;

    leaf->insert(key, value);

    if (first_key_may_change && leaf_parent_page_no != IX_NO_PAGE) {
        memcpy(updated_first_key.data(), leaf->get_key(0), file_hdr_->col_tot_len_);
        should_update_parent = true;
    }

    if (leaf->get_size() >= leaf->get_max_size()) {
        auto new_leaf = split(leaf.get());
        std::vector<char> mid_key(file_hdr_->col_tot_len_);
        memcpy(mid_key.data(), new_leaf->get_key(0), file_hdr_->col_tot_len_);
        insert_into_parent(leaf.get(), mid_key.data(), new_leaf.get(), transaction);
        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
    }

    page_id_t page_no = leaf->get_page_no();
    unlatch_and_unpin_exclusive(leaf, true);

    if (should_update_parent) {
        maintain_parent(leaf_page_no, leaf_parent_page_no, updated_first_key.data());
    }
    return page_no;
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 * @param key 要删除的key值
 * @param transaction 事务指针
 */
bool IxIndexHandle::delete_entry(const char *key, const Rid &rid, Transaction *transaction) {
    auto access_guard = guard_access();
    // Lazy delete removes only the target entry; page identity and right links are preserved.
    auto target = find_leaf_page(key, Operation::DELETE, transaction);
    target->RUnlatch();
    target->WLatch();
    while (target->is_deleted() || should_move_right(target.get(), key)) {
        move_right_with_exclusive_latch(target);
    }
    target = backtrack_leaf(std::move(target), key, LatchMode::EXCLUSIVE);

    bool found = false;
    while (true) {
        found = target->remove(key, rid);
        if (found) {
            break;
        }

        page_id_t next_pid = target->get_next();
        if (next_pid == IX_LEAF_HEADER_PAGE || next_pid == IX_NO_PAGE) {
            unlatch_and_unpin_exclusive(target, false);
            return false;
        }

        auto next = fetch_node(next_pid);
        next->WLatch();
        while (next->is_deleted() || should_move_right(next.get(), key)) {
            move_right_with_exclusive_latch(next);
        }
        if (next->get_size() == 0 ||
            ix_compare(next->get_key(0), key, file_hdr_->col_types_, file_hdr_->col_lens_) > 0) {
            unlatch_and_unpin_exclusive(next, false);
            unlatch_and_unpin_exclusive(target, false);
            return false;
        }

        unlatch_and_unpin_exclusive(target, false);
        target = std::move(next);
    }

    if (target->get_size() > 0 && target->get_parent() != IX_NO_PAGE) {
        std::vector<char> updated_first_key(file_hdr_->col_tot_len_);
        memcpy(updated_first_key.data(), target->get_key(0), file_hdr_->col_tot_len_);
        page_id_t child_page_no = target->get_page_no();
        page_id_t parent_page_no = target->get_parent();
        unlatch_and_unpin_exclusive(target, true);
        maintain_parent(child_page_no, parent_page_no, updated_first_key.data());
        return true;
    }

    unlatch_and_unpin_exclusive(target, true);
    return true;
}
/**
 * @brief Walk backward from `start` to the first leftmost candidate leaf for key
 * Unpins pages it passes through. Returns a pinned leaf.
 */
std::unique_ptr<IxNodeHandle> IxIndexHandle::backtrack_leaf(std::unique_ptr<IxNodeHandle> start, const char *key,
                                                             LatchMode latch_mode) {
    int visited = 0;
    int visit_limit = get_num_pages();
    while (true) {
        if (++visited > visit_limit) {
            if (latch_mode == LatchMode::EXCLUSIVE) {
                unlatch_and_unpin_exclusive(start, false);
            } else {
                unlatch_and_unpin_shared(start);
            }
            throw InternalError("B-link leaf backtrack retry limit reached");
        }
        page_id_t prev_pid = start->get_prev();
        if (prev_pid == IX_LEAF_HEADER_PAGE || prev_pid == IX_NO_PAGE) break;
        if (start->is_deleted()) break;
        page_id_t start_pid = start->get_page_no();
        auto prev = fetch_node(prev_pid);
        if (latch_mode == LatchMode::EXCLUSIVE) {
            prev->WLatch();
        } else {
            prev->RLatch();
        }
        while (prev->is_deleted() || should_move_right(prev.get(), key)) {
            if (prev->get_next() == start_pid) {
                break;
            }
            if (latch_mode == LatchMode::EXCLUSIVE) {
                move_right_with_exclusive_latch(prev);
            } else {
                move_right_with_shared_latch(prev);
            }
        }
        if (prev->get_size() > 0) {
            char *last = prev->get_key(prev->get_size() - 1);
            if (ix_compare(last, key, file_hdr_->col_types_, file_hdr_->col_lens_) >= 0) {
                if (latch_mode == LatchMode::EXCLUSIVE) {
                    unlatch_and_unpin_exclusive(start, false);
                } else {
                    unlatch_and_unpin_shared(start);
                }
                start = std::move(prev);
                continue;
            }
        }
        if (latch_mode == LatchMode::EXCLUSIVE) {
            unlatch_and_unpin_exclusive(prev, false);
        } else {
            unlatch_and_unpin_shared(prev);
        }
        break;
    }
    return start;
}

/**
 * @brief 这里把iid转换成了rid，即iid的slot_no作为node的rid_idx(key_idx)
 * node其实就是把slot_no作为键值对数组的下标
 * 换而言之，每个iid对应的索引槽存了一对(key,rid)，指向了(要建立索引的属性首地址,插入/删除记录的位置)
 *
 * @param iid
 * @return Rid
 * @note iid和rid存的不是一个东西，rid是上层传过来的记录位置，iid是索引内部生成的索引槽位置
 */
Rid IxIndexHandle::get_rid(const Iid &iid) const {
    auto node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        throw IndexEntryNotFoundError();
    }
    auto rid = *node->get_rid(iid.slot_no);
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return rid;
}

/**
 * @brief FindLeafPage + lower_bound
 *
 * @param key
 * @return Iid
 * @note 上层传入的key本来是int类型，通过(const char *)&key进行了转换
 * 可用*(int *)key转换回去
 */
Iid IxIndexHandle::lower_bound(const char *key) {
    auto access_guard = guard_access();
    if (is_empty()) {
        return Iid{-1, -1};
    }
    auto leaf = find_leaf_page(key, Operation::FIND, nullptr);

    int pos = leaf->lower_bound(key);
    if (pos == 0) {
        while (true) {
            page_id_t prev_pid = leaf->get_prev();
            if (leaf->is_deleted()) {
                break;
            }
            if (prev_pid == IX_LEAF_HEADER_PAGE || prev_pid == IX_NO_PAGE) {
                break;
            }

            auto prev = fetch_node(prev_pid);
            prev->RLatch();
            if (prev->is_deleted()) {
                unlatch_and_unpin_shared(prev);
                break;
            }
            if (prev->get_size() == 0 ||
                ix_compare(prev->get_key(prev->get_size() - 1), key, file_hdr_->col_types_, file_hdr_->col_lens_) < 0) {
                unlatch_and_unpin_shared(prev);
                break;
            }

            unlatch_and_unpin_shared(leaf);
            leaf = std::move(prev);
            pos = leaf->lower_bound(key);
            if (pos != 0) {
                break;
            }
        }
    }

    // 向前扫描，处理跨叶边界
    while (leaf->is_deleted() || pos >= leaf->get_size()) {
        if (!leaf->has_next()) {
            Iid iid{leaf->get_page_no(), leaf->get_size()};
            unlatch_and_unpin_shared(leaf);
            return iid;
        }
        move_right_with_shared_latch(leaf);
        pos = leaf->lower_bound(key);
    }
    Iid iid{leaf->get_page_no(), pos};
    unlatch_and_unpin_shared(leaf);
    return iid;
}

/**
 * @brief FindLeafPage + upper_bound
 *
 * @param key
 * @return Iid
 */
Iid IxIndexHandle::upper_bound(const char *key) {
    auto access_guard = guard_access();
    if (is_empty()) {
        return Iid{-1, -1};
    }
    auto leaf = find_leaf_page(key, Operation::FIND, nullptr);
    int pos = leaf->upper_bound(key);
    while (leaf->is_deleted() || pos >= leaf->get_size()) {
        if (!leaf->has_next()) {
            Iid iid{leaf->get_page_no(), leaf->get_size()};
            unlatch_and_unpin_shared(leaf);
            return iid;
        }
        move_right_with_shared_latch(leaf);
        pos = leaf->upper_bound(key);
    }
    Iid iid{leaf->get_page_no(), pos};
    unlatch_and_unpin_shared(leaf);
    return iid;
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 * 用处在于可以作为IxScan的最后一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_end() const {
    page_id_t last_leaf = get_last_leaf_page_no();
    auto node = fetch_node(last_leaf);
    Iid iid = {.page_no = last_leaf, .slot_no = node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return iid;
}

/**
 * @brief 指向第一个叶子的第一个结点
 * 用处在于可以作为IxScan的第一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_begin() const {
    Iid iid = {.page_no = get_first_leaf_page_no(), .slot_no = 0};
    return iid;
}

bool IxIndexHandle::can_delete_leaf(const IxNodeHandle *leaf) const {
    if (!leaf->is_leaf_page() || leaf->get_size() != 0) {
        return false;
    }
    if (!leaf->is_live() || leaf->is_split_incomplete()) {
        return false;
    }
    if (leaf->get_parent() == IX_NO_PAGE) {
        return false;
    }
    if (leaf->get_next() == IX_LEAF_HEADER_PAGE || leaf->get_next() == IX_NO_PAGE) {
        return false;
    }
    if (leaf->get_page_no() == get_root_page_no()) {
        return false;
    }
    if (leaf->get_page_no() == get_last_leaf_page_no()) {
        return false;
    }
    return true;
}

bool IxIndexHandle::unlink_half_dead_leaf(IxNodeHandle *leaf, IxNodeHandle *parent, int parent_idx, page_id_t epoch) {
    page_id_t leaf_page_no = leaf->get_page_no();
    page_id_t right_page_no = leaf->get_next();
    page_id_t left_page_no = leaf->get_prev();

    auto right = fetch_node(right_page_no);
    right->WLatch();
    if (!right->is_live() || !right->is_leaf_page() || right->get_prev() != leaf_page_no) {
        right->WUnlatch();
        buffer_pool_manager_->unpin_page(right->get_page_id(), false);
        return false;
    }

    std::unique_ptr<IxNodeHandle> left;
    if (left_page_no != IX_LEAF_HEADER_PAGE && left_page_no != IX_NO_PAGE) {
        left = fetch_node(left_page_no);
        left->WLatch();
        if (!left->is_live() || left->get_next() != leaf_page_no) {
            unlatch_and_unpin_exclusive(left, false);
            right->WUnlatch();
            buffer_pool_manager_->unpin_page(right->get_page_id(), false);
            return false;
        }
    }

    parent->erase_pair(parent_idx);
    if (parent_idx == 0 && parent->get_size() > 0) {
        parent->set_key(0, right->get_key(0));
    }

    if (left) {
        left->set_next(right_page_no);
    } else {
        set_first_leaf_page_no(right_page_no);
    }
    right->set_prev(left_page_no);

    leaf->mark_deleted(epoch);
    leaf->set_size(0);
    leaf->set_parent(IX_NO_PAGE);
    leaf->set_prev(IX_NO_PAGE);
    leaf->set_next(right_page_no);
    if (right->has_next()) {
        leaf->set_high_key(right->get_high_key());
    } else if (right->get_size() > 0) {
        leaf->set_high_key(right->get_key(right->get_size() - 1));
    }

    if (left) {
        unlatch_and_unpin_exclusive(left, true);
    }
    right->WUnlatch();
    buffer_pool_manager_->unpin_page(right->get_page_id(), true);
    return true;
}

bool IxIndexHandle::try_delete_empty_leaf(page_id_t leaf_page_no, page_id_t epoch) {
    auto leaf = fetch_node(leaf_page_no);
    leaf->WLatch();
    if (!can_delete_leaf(leaf.get())) {
        unlatch_and_unpin_exclusive(leaf, false);
        return false;
    }

    page_id_t parent_page_no = leaf->get_parent();
    auto parent = fetch_node(parent_page_no);
    parent->WLatch();

    int parent_idx = find_child_index(parent.get(), leaf_page_no);
    if (parent_idx < 0 || parent->get_size() <= 1 ||
        parent->is_split_incomplete() || parent->is_deleted()) {
        unlatch_and_unpin_exclusive(parent, false);
        unlatch_and_unpin_exclusive(leaf, false);
        return false;
    }

    if (!can_delete_leaf(leaf.get())) {
        unlatch_and_unpin_exclusive(parent, false);
        unlatch_and_unpin_exclusive(leaf, false);
        return false;
    }

    leaf->mark_half_dead(epoch);
    bool deleted = unlink_half_dead_leaf(leaf.get(), parent.get(), parent_idx, epoch);
    if (!deleted) {
        leaf->mark_live();
    }
    unlatch_and_unpin_exclusive(parent, deleted);
    unlatch_and_unpin_exclusive(leaf, deleted);
    return deleted;
}

bool IxIndexHandle::try_recycle_deleted_page(page_id_t page_no, page_id_t epoch) {
    auto node = fetch_node(page_no);
    node->WLatch();

    if (active_accessors_.load() != 0) {
        unlatch_and_unpin_exclusive(node, false);
        return false;
    }

    if (!node->is_deleted() || epoch - node->page_hdr->deleted_epoch < IX_PAGE_RECYCLE_DELAY) {
        unlatch_and_unpin_exclusive(node, false);
        return false;
    }
    node->mark_reusable();
    node->page_hdr->next_free_page_no = IX_NO_PAGE;
    unlatch_and_unpin_exclusive(node, true);
    push_free_page_no(page_no);
    return true;
}

int IxIndexHandle::cleanup_empty_leaf_pages() {
    std::lock_guard<std::mutex> cleanup_guard(cleanup_latch_);
    page_id_t epoch = advance_cleanup_epoch();
    int cleaned = 0;
    int limit = get_num_pages();
    for (page_id_t page_no = IX_INIT_ROOT_PAGE; page_no < limit; ++page_no) {
        auto node = fetch_node(page_no);
        node->RLatch();
        IxPageRecycleState state = node->page_hdr->recycle_state;
        bool maybe_empty_leaf = node->is_leaf_page() && node->get_size() == 0 && state == IxPageRecycleState::LIVE;
        unlatch_and_unpin_shared(node);

        if (state == IxPageRecycleState::DELETED) {
            if (try_recycle_deleted_page(page_no, epoch)) {
                cleaned++;
            }
            continue;
        }
        if (maybe_empty_leaf && try_delete_empty_leaf(page_no, epoch)) {
            cleaned++;
        }
    }
    return cleaned;
}

/**
 * @brief 获取一个指定结点
 *
 * @param page_no
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 */
std::unique_ptr<IxNodeHandle> IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    if (page == nullptr) {
        throw BufferPoolExhaustedError();
    }
    return std::unique_ptr<IxNodeHandle>(new IxNodeHandle(file_hdr_, page));
}

/**
 * @brief 创建一个新结点
 *
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 * 注意：对于Index的处理是，删除某个页面后，认为该被删除的页面是free_page
 * 而first_free_page实际上就是最新被删除的页面，初始为IX_NO_PAGE
 * 在最开始插入时，一直是create node，那么first_page_no一直没变，一直是IX_NO_PAGE
 * 与Record的处理不同，Record将未插入满的记录页认为是free_page
 */
std::unique_ptr<IxNodeHandle> IxIndexHandle::create_node() {
    page_id_t reusable_page_no = pop_free_page_no();
    PageId new_page_id = {.fd = fd_, .page_no = reusable_page_no};
    Page *page;
    if (reusable_page_no != IX_NO_PAGE) {
        page = buffer_pool_manager_->fetch_page(new_page_id);
        if (page == nullptr) {
            throw BufferPoolExhaustedError();
        }
    } else {
        // Page number allocation and header re onepage count update sha critical region.
        std::lock_guard<std::mutex> create_guard(create_node_latch_);
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
        new_page_id.page_no = INVALID_PAGE_ID;
        page = buffer_pool_manager_->new_page(&new_page_id);
        if (page == nullptr) {
            throw BufferPoolExhaustedError();
        }
        file_hdr_->num_pages_++;
    }

    auto node = std::unique_ptr<IxNodeHandle>(new IxNodeHandle(file_hdr_, page));
    node->page_hdr->next_free_page_no = IX_NO_PAGE;
    node->page_hdr->parent = IX_NO_PAGE;
    node->page_hdr->num_key = 0;
    node->page_hdr->is_leaf = false;
    node->page_hdr->prev = IX_NO_PAGE;
    node->page_hdr->next = IX_NO_PAGE;
    node->page_hdr->split_state = IxPageSplitState::NORMAL;
    node->page_hdr->recycle_state = IxPageRecycleState::LIVE;
    node->page_hdr->deleted_epoch = 0;
    return node;
}

/**
 * @brief 从node开始更新其父节点的第一个key，一直向上更新直到根节点
 *
 * @param node
 */
void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    std::vector<char> child_first_key(file_hdr_->col_tot_len_);
    if (node->get_size() == 0) {
        return;
    }
    memcpy(child_first_key.data(), node->get_key(0), file_hdr_->col_tot_len_);
    maintain_parent(node->get_page_no(), node->get_parent(), child_first_key.data());
}

// First-key propagation uses page numbers and does not require holding the child latch.
void IxIndexHandle::maintain_parent(page_id_t child_page_no, page_id_t parent_page_no, const char *child_first_key) {
    std::vector<char> curr_first_key(file_hdr_->col_tot_len_);
    memcpy(curr_first_key.data(), child_first_key, file_hdr_->col_tot_len_);

    page_id_t curr_child_page_no = child_page_no;
    page_id_t curr_parent_page_no = parent_page_no;

    while (curr_parent_page_no != IX_NO_PAGE) {
        auto parent = fetch_node(curr_parent_page_no);
        parent->WLatch();
        while (parent->is_deleted() || should_move_right(parent.get(), curr_first_key.data())) {
            move_right_with_exclusive_latch(parent);
        }

        int rank = -1;
        for (int i = 0; i < parent->get_size(); ++i) {
            if (parent->get_rid(i)->page_no == curr_child_page_no) {
                rank = i;
                break;
            }
        }

        if (rank < 0) {
            unlatch_and_unpin_exclusive(parent, false);
            break;
        }

        char *parent_key = parent->get_key(rank);
        if (memcmp(parent_key, curr_first_key.data(), file_hdr_->col_tot_len_) == 0) {
            unlatch_and_unpin_exclusive(parent, false);
            break;
        }

        memcpy(parent_key, curr_first_key.data(), file_hdr_->col_tot_len_);

        curr_child_page_no = parent->get_page_no();
        curr_parent_page_no = parent->get_parent();
        memcpy(curr_first_key.data(), parent->get_key(0), file_hdr_->col_tot_len_);

        unlatch_and_unpin_exclusive(parent, true);
    }
}

int IxIndexHandle::find_child_index(const IxNodeHandle *parent, page_id_t child_page_no) const {
    for (int i = 0; i < parent->get_size(); ++i) {
        if (parent->get_rid(i)->page_no == child_page_no) {
            return i;
        }
    }
    return -1;
}

page_id_t IxIndexHandle::locate_parent_page_from(page_id_t page_no, page_id_t child_page_no,
                                                 const char *separator_key) {
    auto node = fetch_node(page_no);
    node->RLatch();

    while (node->is_deleted() || should_move_right(node.get(), separator_key)) {
        page_id_t next_page_no = node->get_next();
        unlatch_and_unpin_shared(node);
        node = fetch_node(next_page_no);
        node->RLatch();
    }

    if (node->is_leaf_page()) {
        unlatch_and_unpin_shared(node);
        return IX_NO_PAGE;
    }

    if (find_child_index(node.get(), child_page_no) >= 0) {
        page_id_t parent_page_no = node->get_page_no();
        unlatch_and_unpin_shared(node);
        return parent_page_no;
    }

    page_id_t next_page_no = node->internal_lookup_ub(separator_key);
    unlatch_and_unpin_shared(node);
    return locate_parent_page_from(next_page_no, child_page_no, separator_key);
}

page_id_t IxIndexHandle::locate_parent_page_from_root(page_id_t child_page_no, const char *separator_key) {
    page_id_t parent_page_no = locate_parent_page_from(get_root_page_no(), child_page_no, separator_key);
    if (parent_page_no == IX_NO_PAGE) {
        throw InternalError("B-link parent relocation failed");
    }
    return parent_page_no;
}

IxIndexHandle::VerifiedParent IxIndexHandle::latch_parent_containing_child(page_id_t child_page_no,
                                                                          const char *separator_key,
                                                                          page_id_t parent_hint) {
    // Return a write-latched parent that still contains the left child downlink.
    page_id_t candidate = parent_hint;
    int attempts = 0;

    while (attempts++ < IX_PARENT_RELOCATION_RETRY_LIMIT) {
        if (candidate == IX_NO_PAGE) {
            candidate = locate_parent_page_from_root(child_page_no, separator_key);
        }

        auto parent = fetch_node(candidate);
        parent->WLatch();
        while (parent->is_deleted() || should_move_right(parent.get(), separator_key)) {
            page_id_t next_page_no = parent->get_next();
            unlatch_and_unpin_exclusive(parent, false);
            parent = fetch_node(next_page_no);
            parent->WLatch();
        }

        int child_idx = find_child_index(parent.get(), child_page_no);
        if (child_idx >= 0) {
            return VerifiedParent{std::move(parent), child_idx};
        }

        unlatch_and_unpin_exclusive(parent, false);
        candidate = locate_parent_page_from_root(child_page_no, separator_key);
    }

    throw InternalError("B-link parent relocation retry limit reached");
}

bool IxIndexHandle::should_move_right(const IxNodeHandle *node, const char *key) const {
    if (!node->has_next()) {
        return false;
    }
    if (node->is_deleted()) {
        return true;
    }
    return ix_compare(key, node->get_high_key(), file_hdr_->col_types_, file_hdr_->col_lens_) >= 0;
}

void IxIndexHandle::unlatch_and_unpin_shared(std::unique_ptr<IxNodeHandle> &node) const {
    if (!node) {
        return;
    }
    node->RUnlatch();
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    node.reset();
}

void IxIndexHandle::unlatch_and_unpin_exclusive(std::unique_ptr<IxNodeHandle> &node, bool is_dirty) const {
    if (!node) {
        return;
    }
    node->WUnlatch();
    buffer_pool_manager_->unpin_page(node->get_page_id(), is_dirty);
    node.reset();
}

void IxIndexHandle::move_right_with_shared_latch(std::unique_ptr<IxNodeHandle> &node) const {
    if (!node->has_next()) {
        throw InternalError("B-link right-link traversal reached end");
    }
    auto next = fetch_node(node->get_next());
    next->RLatch();
    unlatch_and_unpin_shared(node);
    node = std::move(next);
}

void IxIndexHandle::move_right_with_exclusive_latch(std::unique_ptr<IxNodeHandle> &node) const {
    if (!node->has_next()) {
        throw InternalError("B-link right-link traversal reached end");
    }
    auto next = fetch_node(node->get_next());
    next->WLatch();
    unlatch_and_unpin_exclusive(node, false);
    node = std::move(next);
}

/**
 * @brief 要删除leaf之前调用此函数，更新leaf前驱结点的next指针和后继结点的prev指针
 *
 * @param leaf 要删除的leaf
 */
void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    if (!leaf->is_leaf_page()) {
        throw InternalError("erase_leaf: expected leaf page");
    }

    auto prev = fetch_node(leaf->get_prev());
    prev->WLatch();
    prev->set_next(leaf->get_next());
    prev->WUnlatch();
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);

    auto next = fetch_node(leaf->get_next());
    next->WLatch();
    next->set_prev(leaf->get_prev());
    next->WUnlatch();
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
}

void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    node.page_hdr->next_free_page_no = IX_NO_PAGE;
}

void IxIndexHandle::recycle_node_page(IxNodeHandle *node) {
    PageId page_id = node->get_page_id();
    delete node;
    buffer_pool_manager_->unpin_page(page_id, true);
    buffer_pool_manager_->delete_page(page_id);
}

/**
 * @brief 将node的第child_idx个孩子结点的父节点置为node
 */
void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        //  Current node is inner node, load its child and set its parent to current node
        int child_page_no = node->value_at(child_idx);
        auto child = fetch_node(child_page_no);
        child->WLatch();
        child->set_parent(node->get_page_no());
        child->WUnlatch();
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    }
}

bool IxIndexHandle::has_duplicate_keys() const {
    auto access_guard = guard_access();
    Iid iid = leaf_begin();
    Iid end = leaf_end();
    if (iid == end) return false;  // empty index
    if (file_hdr_->col_tot_len_ <= 0) return false;  // invalid key length, treat as no duplicates

    std::vector<char> prev_key(file_hdr_->col_tot_len_);
    bool has_prev = false;

    while (iid != end) {
        auto node = fetch_node(iid.page_no);
        node->RLatch();
        while (iid.slot_no < node->get_size()) {
            const char *key = node->get_key(iid.slot_no);
            if (has_prev && memcmp(prev_key.data(), key, file_hdr_->col_tot_len_) == 0) {
                node->RUnlatch();
                buffer_pool_manager_->unpin_page(node->get_page_id(), false);
                return true;
            }
            memcpy(prev_key.data(), key, file_hdr_->col_tot_len_);
            has_prev = true;
            iid.slot_no++;
        }
        page_id_t next_page = node->get_next();
        bool has_next = node->has_next();
        node->RUnlatch();
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        if (!has_next) break;
        iid.page_no = next_page;
        iid.slot_no = 0;
    }
    return false;
}
