/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "defs.h"
#include "record/rm_defs.h"
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct TabCol {
  std::string tab_name;
  std::string col_name;

  friend bool operator<(const TabCol &x, const TabCol &y) {
    return std::make_pair(x.tab_name, x.col_name) <
           std::make_pair(y.tab_name, y.col_name);
  }
};

inline auto is_numeric_type(ColType type) -> bool {
  return type == TYPE_INT || type == TYPE_FLOAT;
}

template <typename T>
inline auto three_way_compare(const T &lhs, const T &rhs) -> int {
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    return 0;
}


struct Value {
  ColType type; // type of value
  union {
    int int_val;     // int value
    float float_val; // float value
  };
  std::string str_val; // string value

  std::shared_ptr<RmRecord> raw; // raw record buffer

  void set_int(int int_val_) {
    type = TYPE_INT;
    int_val = int_val_;
  }

  void set_float(float float_val_) {
    type = TYPE_FLOAT;
    float_val = float_val_;
  }

  void set_str(std::string str_val_) {
    type = TYPE_STRING;
    str_val = std::move(str_val_);
  }

  void init_raw(int len) {
    assert(raw == nullptr);
    raw = std::make_shared<RmRecord>(len);
    if (type == TYPE_INT) {
      assert(len == sizeof(int));
      *(int *)(raw->data) = int_val;
    } else if (type == TYPE_FLOAT) {
      assert(len == sizeof(float));
      *(float *)(raw->data) = float_val;
    } else if (type == TYPE_STRING) {
      if (len < (int)str_val.size()) {
        throw StringOverflowError();
      }
      memset(raw->data, 0, len);
      memcpy(raw->data, str_val.c_str(), str_val.size());
    }
  }

  inline auto is_numeric() const -> bool { return is_numeric_type(type); }
  
  
  inline auto compare(const Value &rhs) const -> int {
    if (is_numeric() && rhs.is_numeric()) {
      double lhs_num = type == TYPE_INT ? static_cast<double>(int_val)
                                        : static_cast<double>(float_val);
      double rhs_num = rhs.type == TYPE_INT
                           ? static_cast<double>(rhs.int_val)
                           : static_cast<double>(rhs.float_val);
      if (lhs_num < rhs_num) {
        return -1;
      }
      if (lhs_num > rhs_num) {
        return 1;
      }
      return 0;
    }

    if (type == TYPE_STRING && rhs.type == TYPE_STRING) {
      return three_way_compare(str_val, rhs.str_val);
    }

    throw InternalError("Unexpected value type combination in Value::compare");
  }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
  TabCol lhs_col;  // left-hand side column
  CompOp op;       // comparison operator
  bool is_rhs_val; // true if right-hand side is a value (not a column)
  TabCol rhs_col;  // right-hand side column
  Value rhs_val;   // right-hand side value
};

struct SetClause {
  TabCol lhs;
  Value rhs;
};

inline auto are_comparable_types(ColType lhs, ColType rhs) -> bool {
  return lhs == rhs || (is_numeric_type(lhs) && is_numeric_type(rhs));
}

inline auto coerce_value_to_type(const Value &src, ColType target_type,
                                 bool allow_mixed_numeric = false) -> Value {
  Value coerced = src;
  coerced.raw.reset();
  if (src.type == target_type) {
    return coerced;
  }
  if (target_type == TYPE_FLOAT && src.type == TYPE_INT) {
    coerced.set_float(static_cast<float>(src.int_val));
    return coerced;
  }
  if (allow_mixed_numeric && is_numeric_type(target_type) &&
      is_numeric_type(src.type)) {
    return coerced;
  }
  throw IncompatibleTypeError(coltype2str(target_type), coltype2str(src.type));
}
