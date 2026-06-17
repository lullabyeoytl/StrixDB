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

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "defs.h"
#include "errors.h"
#include "system/sm_meta.h"
#include "record/rm_defs.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
    
    inline bool equals(const TabCol &rhs) const {
        return tab_name == rhs.tab_name && col_name == rhs.col_name;
    }
};

struct SortKeySpec {
    TabCol col;
    bool is_desc = false;

    inline auto equals(const SortKeySpec &rhs) const -> bool {
        return col.equals(rhs.col) && is_desc == rhs.is_desc;
    }
};

struct LimitSpec {
    size_t limit = 0;
    size_t offset = 0;
};

// ================================================================
// Column lookup & matching helpers
// Utility functions to search and match TabCol references against ColMeta
// ================================================================
//
inline auto make_sort_key_specs(const std::vector<TabCol> &cols, bool is_desc = false) -> std::vector<SortKeySpec> {
    std::vector<SortKeySpec> keys;
    keys.reserve(cols.size());
    for (const auto &col : cols) {
        keys.push_back(SortKeySpec{col, is_desc});
    }
    return keys;
}

inline bool contains_col(const std::vector<TabCol> &cols, const TabCol &target) {
    return std::any_of(cols.begin(), cols.end(), [&](const TabCol &col) { return col.equals(target); });
}

inline bool col_meta_matches(const ColMeta &col, const TabCol &target) {
    return col.tab_name == target.tab_name && col.name == target.col_name;
}

inline auto find_col_meta(const std::vector<ColMeta> &cols, const TabCol &target) -> const ColMeta & {
    auto it = std::find_if(cols.begin(), cols.end(),
                           [&](const ColMeta &col) { return col_meta_matches(col, target); });
    if (it == cols.end()) {
        throw ColumnNotFoundError("Column not found: " + target.tab_name + "." + target.col_name);
    }
    return *it;
}

// ================================================================
// Type system & comparison enums
// ================================================================
enum AggType {
    AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

inline auto is_numeric_type(ColType type) -> bool {
    return type == TYPE_INT || type == TYPE_FLOAT;
}

inline auto are_comparable_types(ColType lhs, ColType rhs) -> bool {
    return lhs == rhs || (is_numeric_type(lhs) && is_numeric_type(rhs));
}

inline auto is_range_comp_op(CompOp op) -> bool {
    return op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE;
}

inline auto compare_result_matches_op(int cmp, CompOp op) -> bool {
    switch (op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
    }
    throw InternalError("Unexpected value type in compare_values");
}


template <typename T>
inline auto three_way_compare(const T &lhs, const T &rhs) -> int {
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    return 0;
}

const std::map<CompOp, CompOp> kSwapOp = {
    {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
};

// =====================================================
// DATETIME support — calendar arithmetic & literal parsing/formatting
// Internal representation: seconds since epoch (int64_t)
// Covers: leap year detection, days-in-month,
// civil-date ↔ days conversion,
//         'YYYY-MM-DD HH:MM:SS' literal → seconds,
// seconds → formatted string
// =====================================================
static constexpr int kDatetimeLen = static_cast<int>(sizeof(int64_t));
static constexpr int64_t kSecondsPerDay = 24 * 60 * 60;

inline auto is_datetime_leap_year(int year) -> bool {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

inline auto datetime_days_in_month(int year, int month) -> int {
    static constexpr int kMonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_datetime_leap_year(year)) {
        return 29;
    }
    return kMonthDays[month - 1];
}

inline auto datetime_digit(char ch) -> bool {
    return ch >= '0' && ch <= '9';
}

inline auto parse_datetime_part(const std::string &literal, size_t pos, size_t len) -> int {
    int value = 0;
    for (size_t i = 0; i < len; ++i) {
        char ch = literal[pos + i];
        if (!datetime_digit(ch)) {
            throw InvalidDatetimeError(literal);
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

inline auto datetime_days_from_civil(int year, unsigned month, unsigned day) -> int64_t {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

inline void datetime_civil_from_days(int64_t days, int &year, unsigned &month, unsigned &day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp + (mp < 10 ? 3 : -9);
    year += month <= 2;
}

inline auto datetime_literal_to_seconds(const std::string &literal) -> int64_t {
    if (literal.size() != 19 || literal[4] != '-' || literal[7] != '-' || literal[10] != ' ' ||
        literal[13] != ':' || literal[16] != ':') {
        throw InvalidDatetimeError(literal);
    }

    int year = parse_datetime_part(literal, 0, 4);
    int month = parse_datetime_part(literal, 5, 2);
    int day = parse_datetime_part(literal, 8, 2);
    int hour = parse_datetime_part(literal, 11, 2);
    int minute = parse_datetime_part(literal, 14, 2);
    int second = parse_datetime_part(literal, 17, 2);

    if (year < 1 || month < 1 || month > 12 || day < 1 || day > datetime_days_in_month(year, month) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        throw InvalidDatetimeError(literal);
    }

    int64_t days = datetime_days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return days * kSecondsPerDay + hour * 3600 + minute * 60 + second;
}

inline auto format_datetime_value(int64_t seconds) -> std::string {
    int64_t days = seconds / kSecondsPerDay;
    int64_t day_seconds = seconds % kSecondsPerDay;
    if (day_seconds < 0) {
        day_seconds += kSecondsPerDay;
        --days;
    }

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    datetime_civil_from_days(days, year, month, day);

    int hour = static_cast<int>(day_seconds / 3600);
    int minute = static_cast<int>((day_seconds % 3600) / 60);
    int second = static_cast<int>(day_seconds % 60);

    char buffer[20];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u %02d:%02d:%02d", year, month, day, hour, minute, second);
    return std::string(buffer);
}

// ================================================================
// Value — a typed value holder for StrixDB
// ================================================================
struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        float float_val;  // float value
        int64_t datetime_val;
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

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

    void set_datetime(int64_t datetime_val_) {
        type = TYPE_DATETIME;
        datetime_val = datetime_val_;
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
        } else if (type == TYPE_DATETIME) {
            assert(len == kDatetimeLen);
            *reinterpret_cast<int64_t *>(raw->data) = datetime_val;
        } else {
            throw InternalError("Unexpected value type in Value::init_raw");
        }
    }

    inline auto is_numeric() const -> bool {
        return is_numeric_type(type);
    }

    inline auto compare(const Value &rhs) const -> int {
        if (is_numeric() && rhs.is_numeric()) {
            double lhs_num = type == TYPE_INT ? static_cast<double>(int_val) : static_cast<double>(float_val);
            double rhs_num = rhs.type == TYPE_INT ? static_cast<double>(rhs.int_val) : static_cast<double>(rhs.float_val);
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

        if (type == TYPE_DATETIME && rhs.type == TYPE_DATETIME) {
            return three_way_compare(datetime_val, rhs.datetime_val);
        }

        throw InternalError("Unexpected value type combination in Value::compare");
    }

    inline auto equals(const Value &rhs) const -> bool {
        return compare(rhs) == 0;
    }

    friend inline auto operator==(const Value &lhs, const Value &rhs) -> bool {
        return lhs.equals(rhs);
    }

    friend inline auto operator!=(const Value &lhs, const Value &rhs) -> bool {
        return !lhs.equals(rhs);
    }
};

inline auto parse_datetime_literal(const std::string &literal) -> Value {
    Value value;
    value.set_datetime(datetime_literal_to_seconds(literal));
    return value;
}

inline auto get_col_value(const RmRecord &record, const ColMeta &col) -> Value {
    Value value;
    const char *rec_buf = record.data + col.offset;
    if (col.type == TYPE_INT) {
        value.set_int(*reinterpret_cast<const int *>(rec_buf));
    } else if (col.type == TYPE_FLOAT) {
        value.set_float(*reinterpret_cast<const float *>(rec_buf));
    } else if (col.type == TYPE_STRING) {
        std::string str(rec_buf, col.len);
        str.resize(std::strlen(str.c_str()));
        value.set_str(str);
    } else if (col.type == TYPE_DATETIME) {
        value.set_datetime(*reinterpret_cast<const int64_t *>(rec_buf));
    } else {
        throw InternalError("Unexpected column type in get_col_value");
    }
    return value;
}

/**
 * @brief Convert a literal/runtime value to the target column type before materialization.
 *
 * INT -> FLOAT is the only numeric widening conversion.
 * STRING -> DATETIME is allowed only when the target column is datetime.
 * Mixed numeric predicate values can stay in their literal type when allowed.
 */
inline auto coerce_value_to_type(const Value &src, ColType target_type, bool allow_mixed_numeric = false) -> Value {
    Value coerced = src;
    // Force callers to rebuild raw bytes with the destination column length.
    coerced.raw.reset();
    if (src.type == target_type) {
        return coerced;
    }
    if (target_type == TYPE_FLOAT && src.type == TYPE_INT) {
        coerced.set_float(static_cast<float>(src.int_val));
        return coerced;
    }
    if (target_type == TYPE_DATETIME && src.type == TYPE_STRING) {
        return parse_datetime_literal(src.str_val);
    }
    if (allow_mixed_numeric && is_numeric_type(target_type) && coerced.is_numeric()) {
        return coerced;
    }
    throw IncompatibleTypeError(coltype2str(target_type), coltype2str(src.type));
}

// ================================================================
// Condition display & ordering utilities
// Operator symbol, column/value key formatting, canonical sort key,
// and deterministic ordering for condition sets.
// ================================================================
struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
    int lhs_col_idx = -1; // pre-resolved column index in evaluate_columns vector
    int rhs_col_idx = -1; // pre-resolved column index (only valid when !is_rhs_val)

    inline auto equals(const Condition &rhs) const -> bool {
        // 左列， 操作符， RHS种类&值
        if (!lhs_col.equals(rhs.lhs_col) || op != rhs.op || is_rhs_val != rhs.is_rhs_val) return false;
        if (is_rhs_val) {
            return rhs_val.equals(rhs.rhs_val);
        }
        return rhs_col.equals(rhs.rhs_col);
    }
};

inline auto condition_op_symbol(CompOp op) -> std::string {
    switch (op) {
        case OP_EQ:
            return "=";
        case OP_NE:
            return "<>";
        case OP_LT:
            return "<";
        case OP_GT:
            return ">";
        case OP_LE:
            return "<=";
        case OP_GE:
            return ">=";
    }
    throw InternalError("Unexpected comparison operator");
}

inline auto compare_values(const Value &lhs, const Value &rhs, CompOp op) -> bool {
    return compare_result_matches_op(lhs.compare(rhs), op);
}

// Pre-resolve column indices for a condition against a cols vector.
// Call once in executor constructors to avoid O(n_cols) linear scans in hot paths.
inline auto resolve_condition(Condition &cond, const std::vector<ColMeta> &cols) -> void {
    auto lhs_it = std::find_if(cols.begin(), cols.end(),
                               [&](const ColMeta &col) { return col_meta_matches(col, cond.lhs_col); });
    if (lhs_it == cols.end()) {
        throw ColumnNotFoundError(cond.lhs_col.tab_name + '.' + cond.lhs_col.col_name);
    }
    cond.lhs_col_idx = static_cast<int>(lhs_it - cols.begin());
    if (!cond.is_rhs_val) {
        auto rhs_it = std::find_if(cols.begin(), cols.end(),
                                   [&](const ColMeta &col) { return col_meta_matches(col, cond.rhs_col); });
        if (rhs_it == cols.end()) {
            throw ColumnNotFoundError(cond.rhs_col.tab_name + '.' + cond.rhs_col.col_name);
        }
        cond.rhs_col_idx = static_cast<int>(rhs_it - cols.begin());
    }
}

inline auto resolve_conditions(std::vector<Condition> &conds, const std::vector<ColMeta> &cols) -> void {
    for (auto &cond : conds) {
        resolve_condition(cond, cols);
    }
}

inline auto evaluate_condition(const Condition &cond, const RmRecord &record, const std::vector<ColMeta> &cols) -> bool {
    Value lhs_value;
    Value rhs_value;
    if (cond.lhs_col_idx >= 0) {
        lhs_value = get_col_value(record, cols[static_cast<size_t>(cond.lhs_col_idx)]);
    } else {
        lhs_value = get_col_value(record, find_col_meta(cols, cond.lhs_col));
    }
    if (cond.is_rhs_val) {
        rhs_value = cond.rhs_val;
    } else if (cond.rhs_col_idx >= 0) {
        rhs_value = get_col_value(record, cols[static_cast<size_t>(cond.rhs_col_idx)]);
    } else {
        rhs_value = get_col_value(record, find_col_meta(cols, cond.rhs_col));
    }
    return compare_values(lhs_value, rhs_value, cond.op);
}

inline auto evaluate_conditions(const std::vector<Condition> &conds, const RmRecord &record,
                                const std::vector<ColMeta> &cols) -> bool {
    for (const auto &cond : conds) {
        if (!evaluate_condition(cond, record, cols)) {
            return false;
        }
    }
    return true;
}
inline auto render_table_name(const std::string &table,
                              const std::map<std::string, std::string> &table_display_names = {}) -> std::string {
    auto it = table_display_names.find(table);
    return it == table_display_names.end() ? table : it->second;
}

inline auto render_col(const TabCol &col,
                       const std::map<std::string, std::string> &table_display_names = {}) -> std::string {
    if (col.tab_name.empty()) {
        return col.col_name;
    }
    return render_table_name(col.tab_name, table_display_names) + "." + col.col_name;
}

inline auto render_value(const Value &value) -> std::string {
    if (value.type == TYPE_INT) {
        return std::to_string(value.int_val);
    }
    if (value.type == TYPE_FLOAT) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6f", value.float_val);
        return std::string(buffer);
    }
    if (value.type == TYPE_STRING) {
        return "'" + value.str_val + "'";
    }
    if (value.type == TYPE_DATETIME) {
        return "'" + format_datetime_value(value.datetime_val) + "'";
    }
    throw InternalError("Unexpected value type");
}

inline auto render_condition(const Condition &cond,
                             const std::map<std::string, std::string> &table_display_names = {}) -> std::string {
    auto rhs = cond.is_rhs_val ? render_value(cond.rhs_val) : render_col(cond.rhs_col, table_display_names);
    return render_col(cond.lhs_col, table_display_names) + " " + condition_op_symbol(cond.op) + " " + rhs;
}

inline auto render_condition_list(const std::vector<Condition> &conds,
                                  const std::map<std::string, std::string> &table_display_names = {}) -> std::string {
    std::vector<std::string> rendered;
    rendered.reserve(conds.size());
    for (const auto &cond : conds) {
        rendered.push_back(render_condition(cond, table_display_names));
    }
    std::sort(rendered.begin(), rendered.end());

    std::string result = "[";
    for (size_t i = 0; i < rendered.size(); ++i) {
        if (i != 0) {
            result += ", ";
        }
        result += rendered[i];
    }
    result += "]";
    return result;
}

inline auto condition_col_key(const TabCol &col) -> std::string {
    return render_col(col);
}

inline auto condition_value_key(const Value &value) -> std::string {
    return render_value(value);
}

inline auto condition_sort_key(const Condition &cond) -> std::string {
    return render_condition(cond);
}

inline auto condition_less(const Condition &lhs, const Condition &rhs) -> bool {
    return condition_sort_key(lhs) < condition_sort_key(rhs);
}

inline void sort_conditions(std::vector<Condition> &conds) {
    std::sort(conds.begin(), conds.end(), condition_less);
}

struct SetClause {
    TabCol lhs;
    Value rhs;
};

// ================================================================
// Aggregation information — type, column, and star-aggregation flag
// ================================================================

struct AggInfo {
    AggType agg_type;
    bool is_star;
    TabCol col;

    inline auto equals(const AggInfo &rhs) const -> bool {
        if (agg_type != rhs.agg_type || is_star != rhs.is_star) {
            return false;
        }
        if (is_star) {
            return true;
        }
        return col.equals(rhs.col);
    }
};

struct HavingCond {
    bool is_agg;
    AggInfo agg;
    TabCol col;
    CompOp op;
    Value rhs_val;
};

inline std::string agg_output_name(const AggInfo &agg) {
    static const std::map<AggType, std::string> names = {
        {AGG_COUNT, "count"},
        {AGG_SUM, "sum"},
        {AGG_AVG, "avg"},
        {AGG_MIN, "min"},
        {AGG_MAX, "max"},
    };
    if (agg.is_star) {
        return names.at(agg.agg_type) + "(*)";
    }
    return names.at(agg.agg_type) + "(" + agg.col.col_name + ")";
}
