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
#include <shared_mutex>
#include <cassert>
#include <thread>
#include <unordered_map>

class PageLatch {
   public:
    PageLatch() = default;
    ~PageLatch() = default;

    PageLatch(const PageLatch&) = delete;
    PageLatch& operator=(const PageLatch&) = delete;
    PageLatch(PageLatch&&) = delete;
    PageLatch& operator=(PageLatch&&) = delete;

    void lock_shared() {
#ifndef NDEBUG
        const std::thread::id self = std::this_thread::get_id();
        if (debug_state_) {
            std::lock_guard<std::mutex> guard(debug_state_->debug_latch);
            assert(!debug_state_->exclusive_locked || debug_state_->exclusive_owner != self);
        }
#endif

        latch_.lock_shared();

#ifndef NDEBUG
        if (debug_state_) {
            std::lock_guard<std::mutex> guard(debug_state_->debug_latch);
            debug_state_->shared_holders[self]++;
        }
#endif
    }

    void unlock_shared() {
#ifndef NDEBUG
        const std::thread::id self = std::this_thread::get_id();
        if (debug_state_) {
            std::lock_guard<std::mutex> guard(debug_state_->debug_latch);
            auto holder = debug_state_->shared_holders.find(self);
            assert(holder != debug_state_->shared_holders.end());
            assert(holder->second > 0);
            holder->second--;
            if (holder->second == 0) {
                debug_state_->shared_holders.erase(holder);
            }
        }
#endif

        latch_.unlock_shared();
    }

    void lock_exclusive() {
#ifndef NDEBUG
        const std::thread::id self = std::this_thread::get_id();
        if (debug_state_) {
            std::lock_guard<std::mutex> guard(debug_state_->debug_latch);
            assert(!debug_state_->exclusive_locked || debug_state_->exclusive_owner != self);
            assert(debug_state_->shared_holders.find(self) == debug_state_->shared_holders.end());
        }
#endif

        latch_.lock();

#ifndef NDEBUG
        if (debug_state_) {
            std::lock_guard<std::mutex> guard(debug_state_->debug_latch);
            debug_state_->exclusive_locked = true;
            debug_state_->exclusive_owner = self;
        }
#endif
    }

    void unlock_exclusive() {
#ifndef NDEBUG
        const std::thread::id self = std::this_thread::get_id();
        if (debug_state_) {
            std::lock_guard<std::mutex> guard(debug_state_->debug_latch);
            assert(debug_state_->exclusive_locked);
            assert(debug_state_->exclusive_owner == self);
            debug_state_->exclusive_locked = false;
            debug_state_->exclusive_owner = std::thread::id{};
        }
#endif

        latch_.unlock();
    }

   private:
    std::shared_mutex latch_;

    struct DebugState {
        std::mutex debug_latch;
        bool exclusive_locked = false;
        std::thread::id exclusive_owner{};
        std::unordered_map<std::thread::id, size_t> shared_holders;
    };
    // Keep layout stable across translation units even if they disagree on NDEBUG.
    std::unique_ptr<DebugState> debug_state_{
#ifndef NDEBUG
        new DebugState()
#else
        nullptr
#endif
    };
};
