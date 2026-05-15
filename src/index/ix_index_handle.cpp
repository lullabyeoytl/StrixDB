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
    // 1. 校验pos范围
    assert(pos >= 0 && pos <= page_hdr->num_key);
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
    // 校验pos范围
    assert(pos >= 0 && pos < page_hdr->num_key);
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
std::pair<std::unique_ptr<IxNodeHandle>, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    page_id_t page_no = get_root_page_no();
    auto node = fetch_node(page_no);
    node->RLatch();

    while (true) {
        while (!node->is_leaf_page() && should_move_right(node.get(), key)) {
            move_right_with_shared_latch(node);
        }
        if (node->is_leaf_page()) {
            break;
        }

        page_id_t child_page_no = node->internal_lookup_ub(key);
        auto child = fetch_node(child_page_no);
        child->RLatch();
        unlatch_and_unpin_shared(node);
        node = std::move(child);
    }

    while (should_move_right(node.get(), key)) {
        move_right_with_shared_latch(node);
    }

    return {std::move(node), false};
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
    // 1. 创建右兄弟节点
    auto new_node = create_node();
    new_node->page_hdr->is_leaf = node->is_leaf_page();
    new_node->set_parent(node->get_parent());

    int ct_len = file_hdr_->col_tot_len_;
    int total = node->get_size();
    int mid = total / 2;               // left: [0, mid); right: [mid, total)
    int right_cnt = total - mid;
    assert(right_cnt > 0);
    std::vector<char> old_high_key(ct_len);
    bool old_has_next = node->has_next();
    if (old_has_next) {
        memcpy(old_high_key.data(), node->get_high_key(), ct_len);
    }

    // 2. 批量 memcpy 右半部分到新节点
    memcpy(new_node->keys, node->keys + mid * ct_len, right_cnt * ct_len);
    memcpy(new_node->rids, node->rids + mid, right_cnt * sizeof(Rid));
    new_node->set_size(right_cnt);
    node->set_size(mid);
    new_node->set_next(node->get_next());
    node->set_next(new_node->get_page_no());
    node->set_high_key(new_node->get_key(0));
    // adjust high key
    if (new_node->has_next() && old_has_next) {
        new_node->set_high_key(old_high_key.data());
    }

    // 3. 更新双向链表指针
    if (node->is_leaf_page()) {
        new_node->set_prev(node->get_page_no());
        // for b-link tree, we should update next leaf's prev pointer not only for leaf
        if (new_node->get_next() != IX_NO_PAGE && new_node->get_next() != IX_LEAF_HEADER_PAGE) {
            auto old_next = fetch_node(new_node->get_next());
            old_next->WLatch();
            old_next->set_prev(new_node->get_page_no());
            old_next->WUnlatch();
            buffer_pool_manager_->unpin_page(old_next->get_page_id(), true);
        }
        if (get_last_leaf_page_no() == node->get_page_no()) {
            set_last_leaf_page_no(new_node->get_page_no());
        }
    } else {
        new_node->set_prev(IX_NO_PAGE);
        // 4. 内部节点：更新新节点所有孩子的父指针
        for (int i = 0; i < right_cnt; i++) {
            maintain_child(new_node.get(), i);
        }
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
        // old_node 是根，创建新根：内部节点，2 个孩子
        auto new_root = create_node();
        new_root->WLatch();
        new_root->page_hdr->is_leaf = false;
        new_root->set_parent(IX_NO_PAGE);
        // 内部节点约定：keys[i] = rids[i] 子树的第一 key
        // num_key=2: keys[0] 左子树第一 key(不路由), keys[1] 为分隔 key(路由)
        new_root->set_size(2);
        memcpy(new_root->get_key(0), old_node->get_key(0), file_hdr_->col_tot_len_);
        memcpy(new_root->get_key(1), key, file_hdr_->col_tot_len_);
        new_root->set_rid(0, Rid{old_node->get_page_no(), 0});
        new_root->set_rid(1, Rid{new_node->get_page_no(), 0});
        old_node->set_parent(new_root->get_page_no());
        new_node->set_parent(new_root->get_page_no());
        update_root_page_no(new_root->get_page_no());
        new_root->WUnlatch();
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        old_node->WUnlatch();
        return;
    }

    // 保存不可变信息，之后释放 old_node 的锁
    page_id_t old_page_no = old_node->get_page_no();
    page_id_t parent_page_no = old_node->get_parent();

    // 释放子节点锁，再拿父节点锁（避免向上锁耦合）
    old_node->WUnlatch();

    // 非根：找到父节点，在 old_node 位置之后插入 (key, new_node)
    auto parent = fetch_node(parent_page_no);
    parent->WLatch();
    while (should_move_right(parent.get(), key)) {
        move_right_with_exclusive_latch(parent);
    }

    // 用保存的 page_no 定位 child 在 parent 中的位置
    int idx;
    for (idx = 0; idx < parent->get_size(); idx++) {
        if (parent->get_rid(idx)->page_no == old_page_no) {
            break;
        }
    }
    assert(idx < parent->get_size());

    parent->insert_pair(idx + 1, key, Rid{new_node->get_page_no(), 0});
    new_node->set_parent(parent->get_page_no());

    // 父节点溢出则递归分裂
    if (parent->get_size() >= parent->get_max_size()) {
        auto new_parent = split(parent.get());
        char *mid_key = new_parent->get_key(0);
        insert_into_parent(parent.get(), mid_key, new_parent.get(), transaction);
        parent->WLatch();
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
    }

    unlatch_and_unpin_exclusive(parent, true);
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param (key, value) 要插入的键值对
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    std::lock_guard<std::mutex> write_guard(write_path_latch_);
    auto [leaf, _] = find_leaf_page(key, Operation::INSERT, transaction);
    leaf->RUnlatch();
    leaf->WLatch();

    while (should_move_right(leaf.get(), key)) {
        move_right_with_exclusive_latch(leaf);
    }

    // Uniqueness check folded into the same traversal — no separate get_value() call.
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

    // fix: standard strategy: insert first, then split
    leaf->insert(key, value);

    if (first_key_may_change && leaf_parent_page_no != IX_NO_PAGE) {
        memcpy(updated_first_key.data(), leaf->get_key(0), file_hdr_->col_tot_len_);
        should_update_parent = true;
    }

    if (leaf->get_size() >= leaf->get_max_size()) {
        auto new_leaf = split(leaf.get());
        char *mid_key = new_leaf->get_key(0);
        insert_into_parent(leaf.get(), mid_key, new_leaf.get(), transaction);
        leaf->WLatch();
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
    std::lock_guard<std::mutex> write_guard(write_path_latch_);
    // 1. 获取可能包含该键值对的叶子结点
    auto [target, _] = find_leaf_page(key, Operation::DELETE, transaction);
    target->RUnlatch();
    target->WLatch();
    while (should_move_right(target.get(), key)) {
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
        while (should_move_right(next.get(), key)) {
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

    if (!found) {
        unlatch_and_unpin_exclusive(target, false);
        return false;
    }

    // 4. 如果叶子非空，向上传播首 key 变化
    if (target->get_size() > 0) {
        maintain_parent(target.get());
    }

    // 5. 处理下溢（合并或重分配）
    // 释放闩锁后再调用，避免向上锁耦合（与 insert_into_parent 一致）
    bool root_is_latched = false;
    target->WUnlatch();
    bool deleted = coalesce_or_redistribute(target.get(), transaction, &root_is_latched, false);

    if (!deleted) {
        buffer_pool_manager_->unpin_page(target->get_page_id(), true);
    } else {
        target.release();  // coalesce 内部已回收该节点
    }
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 * @note User needs to first find the sibling of input page.
 * If sibling's size + input page's size >= 2 * page's minsize, then redistribute.
 * Otherwise, merge(Coalesce).
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched,
                                             bool node_is_latched) {
    // 保存不可变信息。若节点已被调用者闩锁，读取后释放（避免向上锁耦合）
    page_id_t node_pid, parent_pid;
    if (node_is_latched) {
        node_pid = node->get_page_no();
        parent_pid = node->get_parent();
        node->WUnlatch();  // 与 insert_into_parent 一致：释放子锁再拿父锁
    } else {
        node->WLatch();
        node_pid = node->get_page_no();
        parent_pid = node->get_parent();
        node->WUnlatch();
    }

    // 1. 根节点 → adjust_root 处理
    if (parent_pid == IX_NO_PAGE) {
        node->WLatch();
        bool deleted = adjust_root(node);
        if (!deleted) {
            node->WUnlatch();  // adjust_root 返回 false 时不负责解闩
        }
        return deleted;
    }

    // 2. 提前检查是否需要处理（快速退出优化）
    node->WLatch();
    if (node->get_size() >= node->get_min_size()) {
        node->WUnlatch();
        return false;
    }
    node->WUnlatch();

    // 3. 获取父节点（此时不持有任何子节点闩锁 → 无死锁）
    auto parent_owner = fetch_node(parent_pid);
    parent_owner->WLatch();
    IxNodeHandle *parent = parent_owner.get();

    // 4. 通过保存的 page_no 定位 node 在 parent 中的位置及邻居
    int index;
    for (index = 0; index < parent->get_size(); index++) {
        if (parent->get_rid(index)->page_no == node_pid) break;
    }
    assert(index < parent->get_size());

    page_id_t neighbor_pid = parent->value_at(index > 0 ? index - 1 : 1);

    // 5. 按全局页号顺序获取两个子节点的独占闩——任意两个并发删除都只能按同一次序持有
    std::unique_ptr<IxNodeHandle> neighbor_owner;
    IxNodeHandle *neighbor;
    if (node_pid < neighbor_pid) {
        node->WLatch();  // node 已固定，重新闩锁
        neighbor_owner = fetch_node(neighbor_pid);
        neighbor_owner->WLatch();
        neighbor = neighbor_owner.get();
    } else {
        neighbor_owner = fetch_node(neighbor_pid);
        neighbor_owner->WLatch();
        node->WLatch();  // node 已固定，重新闩锁
        neighbor = neighbor_owner.get();
    }

    // 6. 再次检查是否需要合并（等待闩锁期间可能已被其他线程修复）
    if (node->get_size() >= node->get_min_size()) {
        neighbor->WUnlatch();
        buffer_pool_manager_->unpin_page(neighbor->get_page_id(), false);
        parent->WUnlatch();
        buffer_pool_manager_->unpin_page(parent->get_page_id(), false);
        node->WUnlatch();
        return false;
    }

    int total = node->get_size() + neighbor->get_size();

    // 7. 可以支撑两个节点 → 重分配
    if (total >= 2 * node->get_min_size()) {
        redistribute(neighbor, node, parent, index);
        neighbor->WUnlatch();
        buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        parent->WUnlatch();
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        node->WUnlatch();
        return false;
    }

    // 8. 合并（coalesce 可能交换/删除指针，需 release 所有权）
    IxNodeHandle *original_node = node;
    bool node_was_deleted = (index > 0);

    parent = parent_owner.release();
    neighbor = neighbor_owner.release();

    bool parent_needs_delete = coalesce(&neighbor, &node, &parent, index, transaction, root_is_latched);

    // coalesce 后: neighbor = 左兄弟（合并后幸存, 已闩锁）, node = 右节点（已被 coalesce 回收）
    // index==0 时 swap 使 neighbor 指向 original_node（幸存），node 指向原 neighbor（已回收）

    // 清理 neighbor：若 neighbor 就是 original_node（swap 情况），只解闩不删除
    if (neighbor != original_node) {
        neighbor->WUnlatch();
        buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        delete neighbor;
    } else {
        // original_node 幸存于 swap — 解闩，由调用者负责 unpin
        neighbor->WUnlatch();
    }

    // 处理 parent 的下溢（递归，遵循相同的锁顺序）
    if (parent_needs_delete) {
        // parent 当前持有闩锁，递归调用内部会统一按页号顺序重新获取
        parent->WUnlatch();
        bool parent_deleted = coalesce_or_redistribute(parent, transaction, root_is_latched, false);
        if (!parent_deleted) {
            buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
            delete parent;
        }
    } else {
        parent->WUnlatch();
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        delete parent;
    }

    return node_was_deleted;
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 * @note size of root page can be less than min size and this method is only called within coalesce_or_redistribute()
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    if (!old_root_node->is_leaf_page()) {
        // 当前内部节点约定是一个条目对应一个孩子，size==1 表示根只有唯一孩子
        if (old_root_node->get_size() == 1) {
            page_id_t child_page_no = old_root_node->value_at(0);
            auto child = fetch_node(child_page_no);
            child->WLatch();
            child->set_parent(IX_NO_PAGE);
            update_root_page_no(child_page_no);
            child->WUnlatch();
            buffer_pool_manager_->unpin_page(child->get_page_id(), true);
            old_root_node->WUnlatch();
            recycle_node_page(old_root_node);
            return true;
        }
        return false;
    }
    // 叶子根节点为空时保留根页，后续插入可以直接复用这棵空树
    if (old_root_node->get_size() == 0) {
        update_root_page_no(old_root_node->get_page_no());
        set_first_leaf_page_no(old_root_node->get_page_no());
        set_last_leaf_page_no(old_root_node->get_page_no());
        old_root_node->set_prev(IX_LEAF_HEADER_PAGE);
        old_root_node->set_next(IX_LEAF_HEADER_PAGE);
        return false;
    }
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 * Redistribute key & value pairs from one page to its sibling page. If index == 0, move sibling page's first key
 * & value pair into end of input "node", otherwise move sibling page's last key & value pair into head of input "node".
 *
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 * @note node是之前刚被删除过一个key的结点
 * index=0，则neighbor是node后继结点，表示：node(left)      neighbor(right)
 * index>0，则neighbor是node前驱结点，表示：neighbor(left)  node(right)
 * 注意更新parent结点的相关kv对
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    int ct_len = file_hdr_->col_tot_len_;
    if (index == 0) {
        // index=0: node是左起第一个，neighbor是右兄弟(index+1)
        // 将neighbor的第一个条目移到node末尾
        std::vector<char> first_key(ct_len);
        memcpy(first_key.data(), neighbor_node->get_key(0), ct_len);
        Rid first_rid = *neighbor_node->get_rid(0);
        int old_num = node->get_size();

        neighbor_node->erase_pair(0);
        node->insert_pair(old_num, first_key.data(), first_rid);

        // 更新parent中neighbor的separator key
        parent->set_key(index + 1, neighbor_node->get_key(0));

        if (!node->is_leaf_page()) {
            maintain_child(node, old_num);
        }
    } else {
        // index>0: neighbor是左兄弟(index-1)，node是右兄弟(index)
        // 将neighbor的最后一个条目移到node开头
        int last = neighbor_node->get_size() - 1;
        std::vector<char> last_key(ct_len);
        memcpy(last_key.data(), neighbor_node->get_key(last), ct_len);
        Rid last_rid = *neighbor_node->get_rid(last);

        neighbor_node->erase_pair(last);
        node->insert_pair(0, last_key.data(), last_rid);

        // 更新parent中node的separator key
        parent->set_key(index, node->get_key(0));

        if (!node->is_leaf_page()) {
            maintain_child(node, 0);
        }
    }
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并，也就是和它左边的neighbor_node进行合并；
 * 假设node一定在右边。如果上层传入的index=0，说明node在左边，那么交换node和neighbor_node，保证node在右边；合并到左结点，实际上就是删除了右结点；
 * Move all the key & value pairs from one page to its sibling page, and notify buffer pool manager to delete this page.
 * Parent page must be adjusted to take info of deletion into account. Remember to deal with coalesce or redistribute
 * recursively if necessary.
 *
 * @param neighbor_node sibling page of input "node" (neighbor_node是node的前结点)
 * @param node input from method coalesceOrRedistribute() (node结点是需要被删除的)
 * @param parent parent page of input "node"
 * @param index node在parent中的rid_idx
 * @return true means parent node should be deleted, false means no deletion happend
 * @note Assume that *neighbor_node is the left sibling of *node (neighbor -> node)
 */
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    // 保证 *neighbor_node 为左结点，*node 为右结点
    if (index == 0) {
        // 交换：原来 node 是左起第一个，neighbor 是右兄弟
        IxNodeHandle *tmp = *neighbor_node;
        *neighbor_node = *node;
        *node = tmp;
    }
    IxNodeHandle *left = *neighbor_node;   // 左结点（保留）
    IxNodeHandle *right = *node;           // 右结点（将被删除）

    int ct_len = file_hdr_->col_tot_len_;
    int right_cnt = right->get_size();
    int left_cnt = left->get_size();

    // 1. 将右结点的所有条目批量拷贝到左结点的末尾
    memcpy(left->get_key(left_cnt), right->get_key(0), right_cnt * ct_len);
    memcpy(left->get_rid(left_cnt), right->get_rid(0), right_cnt * sizeof(Rid));
    left->set_size(left_cnt + right_cnt);

    // 2. 更新叶子链表或内部节点孩子父指针
    if (left->is_leaf_page()) {
        // 叶子：将右结点的后继变成左结点的后继
        left->set_next(right->get_next());
        if (right->get_next() != IX_NO_PAGE) {
            auto next = fetch_node(right->get_next());
            next->WLatch();
            next->set_prev(left->get_page_no());
            next->WUnlatch();
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        }
        if (get_last_leaf_page_no() == right->get_page_no()) {
            set_last_leaf_page_no(left->get_page_no());
        }
    } else {
        // 内部：更新所有从右结点移过来的孩子父指针
        for (int i = left_cnt; i < left_cnt + right_cnt; i++) {
            maintain_child(left, i);
        }
    }

    // 3. 删除右结点在父节点中的条目
    int erase_pos = (index == 0) ? 1 : index;
    (*parent)->erase_pair(erase_pos);

    // 4. 释放并删除右结点
    right->WUnlatch();
    recycle_node_page(right);

    // 返回 true：父节点条目被删除，需要进一步处理
    return true;
}

/**
 * @brief Walk backward from `start` to the first leftmost candidate leaf for key
 * Unpins pages it passes through. Returns a pinned leaf.
 */
std::unique_ptr<IxNodeHandle> IxIndexHandle::backtrack_leaf(std::unique_ptr<IxNodeHandle> start, const char *key,
                                                             LatchMode latch_mode) {
    while (true) {
        page_id_t prev_pid = start->get_prev();
        if (prev_pid == IX_LEAF_HEADER_PAGE || prev_pid == IX_NO_PAGE) break;
        page_id_t start_pid = start->get_page_no();
        auto prev = fetch_node(prev_pid);
        if (latch_mode == LatchMode::EXCLUSIVE) {
            prev->WLatch();
        } else {
            prev->RLatch();
        }
        while (should_move_right(prev.get(), key)) {
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
    if (is_empty()) {
        return Iid{-1, -1};
    }
    auto [leaf, _] = find_leaf_page(key, Operation::FIND, nullptr);

    int pos = leaf->lower_bound(key);
    if (pos == 0) {
        while (true) {
            page_id_t prev_pid = leaf->get_prev();
            if (prev_pid == IX_LEAF_HEADER_PAGE || prev_pid == IX_NO_PAGE) {
                break;
            }

            auto prev = fetch_node(prev_pid);
            prev->RLatch();
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
    while (pos >= leaf->get_size()) {
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
    if (is_empty()) {
        return Iid{-1, -1};
    }
    auto [leaf, _] = find_leaf_page(key, Operation::FIND, nullptr);
    int pos = leaf->upper_bound(key);
    while (pos >= leaf->get_size()) {
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
    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page;
    {
        std::lock_guard<std::mutex> guard(file_hdr_latch_);
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
    return node;
}

/**
 * @brief 从node开始更新其父节点的第一个key，一直向上更新直到根节点
 *
 * @param node
 */
void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    std::vector<char> child_first_key(file_hdr_->col_tot_len_);
    memcpy(child_first_key.data(), node->get_key(0), file_hdr_->col_tot_len_);
    maintain_parent(node->get_page_no(), node->get_parent(), child_first_key.data());
}

// 改为接受值参数，不依赖不持有子节点latch
void IxIndexHandle::maintain_parent(page_id_t child_page_no, page_id_t parent_page_no, const char *child_first_key) {
    std::vector<char> curr_first_key(file_hdr_->col_tot_len_);
    memcpy(curr_first_key.data(), child_first_key, file_hdr_->col_tot_len_);

    page_id_t curr_child_page_no = child_page_no;
    page_id_t curr_parent_page_no = parent_page_no;

    while (curr_parent_page_no != IX_NO_PAGE) {
        auto parent = fetch_node(curr_parent_page_no);
        parent->WLatch();
        while (should_move_right(parent.get(), curr_first_key.data())) {
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

bool IxIndexHandle::should_move_right(const IxNodeHandle *node, const char *key) const {
    return node->has_next() && ix_compare(key, node->get_high_key(), file_hdr_->col_types_, file_hdr_->col_lens_) >= 0;
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
    auto next = fetch_node(node->get_next());
    next->RLatch();
    unlatch_and_unpin_shared(node);
    node = std::move(next);
}

void IxIndexHandle::move_right_with_exclusive_latch(std::unique_ptr<IxNodeHandle> &node) const {
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
    assert(leaf->is_leaf_page());

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
    buffer_pool_manager_->unpin_page(page_id, true);
    buffer_pool_manager_->delete_page(page_id);
    delete node;
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
    Iid iid = leaf_begin();
    Iid end = leaf_end();
    if (iid == end) return false;  // empty index
    if (file_hdr_->col_tot_len_ <= 0) return false;  // invalid key length, treat as no duplicates

    std::vector<char> prev_key(file_hdr_->col_tot_len_);
    bool has_prev = false;

    while (iid != end) {
        auto node = fetch_node(iid.page_no);
        while (iid.slot_no < node->get_size()) {
            const char *key = node->get_key(iid.slot_no);
            if (has_prev && memcmp(prev_key.data(), key, file_hdr_->col_tot_len_) == 0) {
                buffer_pool_manager_->unpin_page(node->get_page_id(), false);
                return true;
            }
            memcpy(prev_key.data(), key, file_hdr_->col_tot_len_);
            has_prev = true;
            iid.slot_no++;
        }
        page_id_t next_page = node->get_next();
        bool has_next = node->has_next();
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        if (!has_next) break;
        iid.page_no = next_page;
        iid.slot_no = 0;
    }
    return false;
}
