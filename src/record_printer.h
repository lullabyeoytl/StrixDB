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

#include <cassert>
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <vector>
#include "common/context.h"
#include "common/config.h"

#define RECORD_COUNT_LENGTH 40

class RecordPrinter {
   public:
    static constexpr size_t DEFAULT_COL_WIDTH = 16;
    static constexpr size_t DATETIME_COL_WIDTH = 19;

   private:
    size_t num_cols;
    std::vector<size_t> col_widths;

    void write_to_context(const std::string &str, Context *context) const {
        if (!context->ellipsis_ && *context->offset_ + RECORD_COUNT_LENGTH + str.length() < BUFFER_LENGTH) {
            memcpy(context->data_send_ + *context->offset_, str.c_str(), str.length());
            *context->offset_ += str.length();
        } else {
            context->ellipsis_ = true;
        }
    }

   public:
    RecordPrinter(size_t num_cols_) : num_cols(num_cols_), col_widths(num_cols_, DEFAULT_COL_WIDTH) {
        assert(num_cols_ > 0);
    }

    explicit RecordPrinter(std::vector<size_t> col_widths_)
        : num_cols(col_widths_.size()), col_widths(std::move(col_widths_)) {
        assert(num_cols > 0);
    }

    void print_separator(Context *context) const {
        for (size_t i = 0; i < num_cols; i++) {
            write_to_context("+" + std::string(col_widths[i] + 2, '-'), context);
        }
        write_to_context("+\n", context);
    }

    void print_record(const std::vector<std::string> &rec_str, Context *context) const {
        assert(rec_str.size() == num_cols);
        for (size_t i = 0; i < rec_str.size(); ++i) {
            auto col = rec_str[i];
            size_t col_width = col_widths[i];
            if (col.size() > col_width) {
                col = col.substr(0, col_width - 3) + "...";
            }
            std::stringstream ss;
            ss << "| " << std::setw(static_cast<int>(col_width)) << col << " ";
            write_to_context(ss.str(), context);
        }
        write_to_context("|\n", context);
    }

    static void print_record_count(size_t num_rec, Context *context) {
        // std::cout << "Total record(s): " << num_rec << '\n';
        std::string str = "";
        if(context->ellipsis_ == true) {
            str = "... ...\n";
        }
        str += "Total record(s): " + std::to_string(num_rec) + '\n';
        memcpy(context->data_send_ + *(context->offset_), str.c_str(), str.length());
        *(context->offset_) = *(context->offset_) + str.length();
    }
};
