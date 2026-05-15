/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_manager.h"

#include <array>
#include <mutex>
#include <vector>

#include "recovery/log_manager.h"

/**
 * @description: 从free_list或replacer中得到可淘汰帧页的 *frame_id
 * @return {bool} true: 可替换帧查找成功 , false: 可替换帧查找失败
 * @param {frame_id_t*} frame_id 帧页id指针,返回成功找到的可替换帧id
 */
bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    // Todo:
    // 1 使用BufferPoolManager::free_list_判断缓冲池是否已满需要淘汰页面
    // 1.1 未满获得frame
    // 1.2 已满使用lru_replacer中的方法选择淘汰页面
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    return replacer_->victim(frame_id);
}

/**
 * @description: 更新页面数据, 如果为脏页则需写入磁盘，再更新为新页面，更新page元数据(data, is_dirty, page_id)和page table
 * @param {Page*} page 写回页指针
 * @param {PageId} new_page_id 新的page_id
 * @param {frame_id_t} new_frame_id 新的帧frame_id
 */
void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    if (frame_keys_[new_frame_id].page_no != INVALID_PAGE_ID) {
        page_table_.erase(frame_keys_[new_frame_id]);
    }
    PageKey new_page_key = make_page_key(new_page_id);
    page->reset_memory();
    page->id_ = new_page_id;
    page->is_dirty_ = false;
    page->pin_count_ = 0;
    frame_keys_[new_frame_id] = new_page_key;
    page_table_[new_page_key] = new_frame_id;
    frame_states_[new_frame_id] = FrameState::READY;
}

BufferPoolManager::PageKey BufferPoolManager::make_page_key(PageId page_id) {
    return PageKey{disk_manager_->get_file_name(page_id.fd), page_id.page_no};
}

void BufferPoolManager::flush_wal_before_page_write(const char *data) {
    if (log_manager_ == nullptr || data == nullptr) {
        return;
    }
    lsn_t page_lsn = *reinterpret_cast<const lsn_t *>(data + Page::OFFSET_LSN);
    log_manager_->flush_log_to_lsn(page_lsn);
}

void BufferPoolManager::write_page_data(const PageKey &page_key, const char *data) {
    if (page_key.page_no == INVALID_PAGE_ID) {
        return;
    }

    flush_wal_before_page_write(data);
    const bool was_open = disk_manager_->is_file_open(page_key.file_name);
    int fd = disk_manager_->get_file_fd(page_key.file_name);
    disk_manager_->write_page(fd, page_key.page_no, data, PAGE_SIZE);
    if (!was_open) {
        disk_manager_->close_file(fd);
    }
}

/**
 * @description: 从buffer pool获取需要的页。
 *              如果页表中存在page_id（说明该page在缓冲池中），并且pin_count++。
 *              如果页表不存在page_id（说明该page在磁盘中），则找缓冲池victim page，将其替换为磁盘中读取的page，pin_count置1。
 * @return {Page*} 若获得了需要的页则将其返回，否则返回nullptr
 * @param {PageId} page_id 需要获取的页的PageId
 */
Page* BufferPoolManager::fetch_page(PageId page_id) {
    // 1.     从page_table_中搜寻目标页
    // 1.1    若目标页有被page_table_记录，则将其所在frame固定(pin)，并返回目标页。
    // 1.2    否则，尝试调用find_victim_page获得一个可用的frame，若失败则返回nullptr
    // 2.     若获得的可用frame存储的为dirty page，则须调用updata_page将page写回到磁盘
    // 3.     调用disk_manager_的read_page读取目标页到frame
    // 4.     固定目标页，更新pin_count_
    // 5.     返回目标页
    PageKey page_key = make_page_key(page_id);
    frame_id_t frame_id = INVALID_FRAME_ID;
    PageKey victim_page_key;
    bool flush_victim = false;
    std::array<char, PAGE_SIZE> victim_data{};
    std::array<char, PAGE_SIZE> page_data{};

    {
        std::unique_lock<std::mutex> lock(latch_);
        while (true) {
            auto it = page_table_.find(page_key);
            if (it != page_table_.end()) {
                frame_id = it->second;
                // 如果目标页在IO, 就释放锁并睡眠，被唤醒后检查限免条件访问&重查
                cv_.wait(lock, [&]() {
                    auto current = page_table_.find(page_key);
                    return current == page_table_.end() || frame_states_[current->second] != FrameState::IO_IN_PROGRESS;
                });
                it = page_table_.find(page_key);
                if (it == page_table_.end()) {
                    // failed to find: 重新走查找
                    continue;
                }
                // go on visiting the page
                Page *page = &pages_[it->second];
                page->id_ = page_id;
                page->pin_count_++;
                replacer_->pin(it->second);
                return page;
            }

            if (!find_victim_page(&frame_id)) {
                return nullptr;
            }

            Page *page = &pages_[frame_id];
            if (frame_keys_[frame_id].page_no != INVALID_PAGE_ID) {
                victim_page_key = frame_keys_[frame_id];
                flush_victim = page->is_dirty();
                if (flush_victim) {
                    memcpy(victim_data.data(), page->get_data(), PAGE_SIZE);
                }
                page_table_.erase(victim_page_key);
            }

            page->id_ = page_id;
            page->pin_count_ = 1;
            page->is_dirty_ = false;
            frame_keys_[frame_id] = page_key;
            frame_states_[frame_id] = FrameState::IO_IN_PROGRESS;
            page_table_[page_key] = frame_id;
            replacer_->pin(frame_id);
            break;
        }
    }

    try {
        if (flush_victim) {
            write_page_data(victim_page_key, victim_data.data());
        }
        disk_manager_->read_page(page_id.fd, page_id.page_no, page_data.data(), PAGE_SIZE);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(latch_);
            auto it = page_table_.find(page_key);
            if (it != page_table_.end() && it->second == frame_id) {
                page_table_.erase(it);
            }
            Page *page = &pages_[frame_id];
            page->reset_memory();
            page->id_ = PageId{-1, INVALID_PAGE_ID};
            page->is_dirty_ = false;
            page->pin_count_ = 0;
            frame_keys_[frame_id] = PageKey{};
            frame_states_[frame_id] = FrameState::FREE;
            free_list_.push_back(frame_id);
        }
        cv_.notify_all();
        throw;
    }

    {
        std::lock_guard<std::mutex> lock(latch_);
        Page *page = &pages_[frame_id];
        memcpy(page->get_data(), page_data.data(), PAGE_SIZE);
        frame_states_[frame_id] = FrameState::READY;
    }
    cv_.notify_all();
    return &pages_[frame_id];
}

/**
 * @description: 取消固定pin_count>0的在缓冲池中的page
 * @return {bool} 如果目标页的pin_count<=0则返回false，否则返回true
 * @param {PageId} page_id 目标page的page_id
 * @param {bool} is_dirty 若目标page应该被标记为dirty则为true，否则为false
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    // 0. lock latch
    // 1. 尝试在page_table_中搜寻page_id对应的页P
    // 1.1 P在页表中不存在 return false
    // 1.2 P在页表中存在，获取其pin_count_
    // 2.1 若pin_count_已经等于0，则返回false
    // 2.2 若pin_count_大于0，则pin_count_自减一
    // 2.2.1 若自减后等于0，则调用replacer_的Unpin
    // 3 根据参数is_dirty，更改P的is_dirty_
    PageKey page_key = make_page_key(page_id);
    std::lock_guard<std::mutex> lock(latch_);
    auto it = page_table_.find(page_key);
    if (it == page_table_.end()) {
        return false;
    } else {
        Page *page = &pages_[it->second];
        if (page->pin_count_ <= 0) {
            return false;
        } else {
            page->pin_count_--;
            if (page->pin_count_ == 0 && frame_states_[it->second] == FrameState::READY) {
                replacer_->unpin(it->second);
            }
            page->is_dirty_ |= is_dirty;
        }
    }
    return true;
}

/**
 * @description: 将目标页写回磁盘，不考虑当前页面是否正在被使用
 * @return {bool} 成功则返回true，否则返回false(只有page_table_中没有目标页时)
 * @param {PageId} page_id 目标页的page_id，不能为INVALID_PAGE_ID
 */
bool BufferPoolManager::flush_page(PageId page_id) {

    // 0. lock latch
    // 1. 查找页表,尝试获取目标页P
    // 1.1 目标页P没有被page_table_记录 ，返回false
    // 2. 无论P是否为脏都将其写回磁盘。
    // 3. 更新P的is_dirty_
    PageKey page_key = make_page_key(page_id);
    frame_id_t frame_id = INVALID_FRAME_ID;
    std::array<char, PAGE_SIZE> page_data{};

    {
        std::unique_lock<std::mutex> lock(latch_);
        while (true) {
            auto it = page_table_.find(page_key);
            if (it == page_table_.end()) {
                return false;
            }
            frame_id = it->second;
            if (frame_states_[frame_id] == FrameState::IO_IN_PROGRESS) {
                // waiting for frame finishing IO
                cv_.wait(lock, [&]() {
                    auto current = page_table_.find(page_key);
                    return current == page_table_.end() || frame_states_[current->second] != FrameState::IO_IN_PROGRESS;
                });
                continue;
            }
            Page *page = &pages_[frame_id];
            if (page->pin_count_ == 0) {
                replacer_->pin(frame_id);
            }
            memcpy(page_data.data(), page->get_data(), PAGE_SIZE);
            page->is_dirty_ = false;
            frame_states_[frame_id] = FrameState::IO_IN_PROGRESS;
            break;
        }
    }

    try {
        write_page_data(page_key, page_data.data());
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(latch_);
            auto it = page_table_.find(page_key);
            if (it != page_table_.end() && it->second == frame_id) {
                Page *page = &pages_[frame_id];
                page->is_dirty_ = true;
                frame_states_[frame_id] = FrameState::READY;
                if (page->pin_count_ == 0) {
                    replacer_->unpin(frame_id);
                }
            }
        }
        cv_.notify_all();
        throw;
    }

    {
        std::lock_guard<std::mutex> lock(latch_);
        auto it = page_table_.find(page_key);
        if (it != page_table_.end() && it->second == frame_id) {
            Page *page = &pages_[frame_id];
            frame_states_[frame_id] = FrameState::READY;
            if (page->pin_count_ == 0) {
                replacer_->unpin(frame_id);
            }
        }
    }
    cv_.notify_all();
    return true;
}

/**
 * @description: 创建一个新的page，即从磁盘中移动一个新建的空page到缓冲池某个位置。
 * @return {Page*} 返回新创建的page，若创建失败则返回nullptr
 * @param {PageId*} page_id 当成功创建一个新的page时存储其page_id
 */
Page* BufferPoolManager::new_page(PageId* page_id) {
    // 1.   获得一个可用的frame，若无法获得则返回nullptr
    // 2.   在fd对应的文件分配一个新的page_id
    // 3.   将frame的数据写回磁盘
    // 4.   固定frame，更新pin_count_
    // 5.   返回获得的page
    frame_id_t frame_id;
    PageKey page_key;
    PageKey victim_page_key;
    bool flush_victim = false;
    std::array<char, PAGE_SIZE> victim_data{};

    {
        std::lock_guard<std::mutex> lock(latch_);
        if (!find_victim_page(&frame_id)) {
            return nullptr;
        }
        page_id->page_no = disk_manager_->allocate_page(page_id->fd);
        page_key = make_page_key(*page_id);
        Page *page = &pages_[frame_id];
        if (frame_keys_[frame_id].page_no != INVALID_PAGE_ID) {
            victim_page_key = frame_keys_[frame_id];
            flush_victim = page->is_dirty();
            if (flush_victim) {
                memcpy(victim_data.data(), page->get_data(), PAGE_SIZE);
            }
            page_table_.erase(victim_page_key);
        }
        page->id_ = *page_id;
        page->pin_count_ = 1;
        page->is_dirty_ = false;
        frame_keys_[frame_id] = page_key;
        frame_states_[frame_id] = FrameState::IO_IN_PROGRESS;
        page_table_[page_key] = frame_id;
        replacer_->pin(frame_id);
    }

    try {
        if (flush_victim) {
            write_page_data(victim_page_key, victim_data.data());
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(latch_);
            auto it = page_table_.find(page_key);
            if (it != page_table_.end() && it->second == frame_id) {
                page_table_.erase(it);
            }
            Page *page = &pages_[frame_id];
            page->reset_memory();
            page->id_ = PageId{-1, INVALID_PAGE_ID};
            page->is_dirty_ = false;
            page->pin_count_ = 0;
            frame_keys_[frame_id] = PageKey{};
            frame_states_[frame_id] = FrameState::FREE;
            free_list_.push_back(frame_id);
        }
        cv_.notify_all();
        throw;
    }

    {
        std::lock_guard<std::mutex> lock(latch_);
        Page *page = &pages_[frame_id];
        page->reset_memory();
        frame_states_[frame_id] = FrameState::READY;
    }
    cv_.notify_all();
    return &pages_[frame_id];
}

/**
 * @description: 从buffer_pool删除目标页
 * @return {bool} 如果目标页不存在于buffer_pool或者成功被删除则返回true，若其存在于buffer_pool但无法删除则返回false
 * @param {PageId} page_id 目标页
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    // 1.   在page_table_中查找目标页，若不存在返回true
    // 2.   若目标页的pin_count不为0，则返回false
    // 3.   将目标页数据写回磁盘，从页表中删除目标页，重置其元数据，将其加入free_list_，返回true
    PageKey page_key = make_page_key(page_id);
    frame_id_t frame_id = INVALID_FRAME_ID;
    std::array<char, PAGE_SIZE> page_data{};

    {
        std::unique_lock<std::mutex> lock(latch_);
        while (true) {
            auto it = page_table_.find(page_key);
            if (it == page_table_.end()) {
                return true;
            }
            frame_id = it->second;
            if (frame_states_[frame_id] == FrameState::IO_IN_PROGRESS) {
                cv_.wait(lock, [&]() {
                    auto current = page_table_.find(page_key);
                    return current == page_table_.end() || frame_states_[current->second] != FrameState::IO_IN_PROGRESS;
                });
                continue;
            }
            if (pages_[frame_id].pin_count_ != 0) {
                return false;
            }
            replacer_->pin(frame_id);
            memcpy(page_data.data(), pages_[frame_id].get_data(), PAGE_SIZE);
            frame_states_[frame_id] = FrameState::IO_IN_PROGRESS;
            break;
        }
    }

    try {
        write_page_data(page_key, page_data.data());
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(latch_);
            auto it = page_table_.find(page_key);
            if (it != page_table_.end() && it->second == frame_id) {
                frame_states_[frame_id] = FrameState::READY;
                replacer_->unpin(frame_id);
            }
        }
        cv_.notify_all();
        throw;
    }

    {
        std::lock_guard<std::mutex> lock(latch_);
        auto it = page_table_.find(page_key);
        if (it != page_table_.end() && it->second == frame_id) {
            page_table_.erase(it);
        }
        Page *page = &pages_[frame_id];
        page->reset_memory();
        page->id_ = PageId{-1, INVALID_PAGE_ID};
        page->is_dirty_ = false;
        page->pin_count_ = 0;
        frame_keys_[frame_id] = PageKey{};
        frame_states_[frame_id] = FrameState::FREE;
        free_list_.push_back(frame_id);
    }
    cv_.notify_all();
    return true;
}

/**
 * @description: 将buffer_pool中的所有页写回到磁盘
 * @param {int} fd 文件句柄
 */
 void BufferPoolManager::flush_all_pages(int fd) {
      std::vector<PageId> pages_to_flush;
      std::string file_name = disk_manager_->get_file_name(fd);
      {
        // also need lock in flush_page, so first get all page_id in vec
          std::lock_guard<std::mutex> lock(latch_);
          for (auto &entry : page_table_) {
              if (entry.first.file_name == file_name) {
                  pages_to_flush.push_back(PageId{fd, entry.first.page_no});
              }
          }
      }
      for (auto &page_id : pages_to_flush) {
          flush_page(page_id);
      }
  }
