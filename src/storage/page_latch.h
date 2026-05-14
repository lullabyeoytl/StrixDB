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

#include <atomic>
#include <cassert>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

class PageLatch {
   public:
    void lock_shared() {
#ifndef NDEBUG
        const std::thread::id self = std::this_thread::get_id();
        {
            std::lock_guard<std::mutex> guard(debug_latch_);
            assert(!exclusive_locked_ || exclusive_owner_ != self);
        }
#endif

        latch_.lock_shared();

#ifndef NDEBUG
        std::lock_guard<std::mutex> guard(debug_latch_);
        shared_holders_[self]++;
#endif
    }

    void unlock_shared() {
#ifndef NDEBUG
        const std::thread::id self = std::this_thread::get_id();
        {
            std::lock_guard<std::mutex> guard(debug_latch_);
            auto holder = shared_holders_.find(self);
            assert(holder != shared_holders_.end());
            assert(holder->second > 0);
            holder->second--;
            if (holder->second == 0) {
                shared_holders_.erase(holder);
            }
        }
#endif

        latch_.unlock_shared();
    }

    void lock_exclusive() {
#ifndef NDEBUG
        const std::thread::id self = std::this_thread::get_id();
        {
            std::lock_guard<std::mutex> guard(debug_latch_);
            assert(!exclusive_locked_ || exclusive_owner_ != self);
            assert(shared_holders_.find(self) == shared_holders_.end());
        }
#endif

        latch_.lock();

#ifndef NDEBUG
        std::lock_guard<std::mutex> guard(debug_latch_);
        exclusive_locked_ = true;
        exclusive_owner_ = self;
#endif
    }

    void unlock_exclusive() {
#ifndef NDEBUG
        const std::thread::id self = std::this_thread::get_id();
        {
            std::lock_guard<std::mutex> guard(debug_latch_);
            assert(exclusive_locked_);
            assert(exclusive_owner_ == self);
            exclusive_locked_ = false;
            exclusive_owner_ = std::thread::id{};
        }
#endif

        latch_.unlock();
    }

   private:
    std::shared_mutex latch_;

#ifndef NDEBUG
    std::mutex debug_latch_;
    std::atomic_bool exclusive_locked_ = false;
    std::thread::id exclusive_owner_{};
    std::unordered_map<std::thread::id, size_t> shared_holders_;
#endif
};
