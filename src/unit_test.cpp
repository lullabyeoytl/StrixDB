/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#undef NDEBUG

#define private public

#include "index/ix.h"
#include "record/rm.h"
#include "storage/buffer_pool_manager.h"

#undef private

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <stdexcept>
#include <thread>  // NOLINT
#include <unordered_map>
#include <vector>

#include "analyze/analyze.h"
#include "execution/execution_sort.h"
#include "execution/executor_hash_join.h"
#include "execution/executor_limit.h"
#include "gtest/gtest.h"
#include "optimizer/planner.h"
#include "parser/parser.h"
#include "replacer/lru_replacer.h"
#include "storage/disk_manager.h"

const std::string TEST_DB_NAME = "BufferPoolManagerTest_db";  // 以数据库名作为根目录
const std::string TEST_FILE_NAME = "basic";                   // 测试文件的名字
const std::string TEST_FILE_NAME_CCUR = "concurrency";        // 测试文件的名字
const std::string TEST_FILE_NAME_BIG = "bigdata";             // 测试文件的名字
constexpr int MAX_FILES = 32;
constexpr int MAX_PAGES = 128;
constexpr size_t TEST_BUFFER_POOL_SIZE = MAX_FILES * MAX_PAGES;

// 创建BufferPoolManager
auto disk_manager = std::make_unique<DiskManager>();
auto buffer_pool_manager = std::make_unique<BufferPoolManager>(TEST_BUFFER_POOL_SIZE, disk_manager.get());

std::unordered_map<int, char *> mock;  // fd -> buffer

char *mock_get_page(int fd, int page_no) { return &mock[fd][page_no * PAGE_SIZE]; }

void check_disk(int fd, int page_no) {
    char buf[PAGE_SIZE];
    disk_manager->read_page(fd, page_no, buf, PAGE_SIZE);
    char *mock_buf = mock_get_page(fd, page_no);
    assert(memcmp(buf, mock_buf, PAGE_SIZE) == 0);
}

void check_disk_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
}

void check_cache(int fd, int page_no) {
    Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
    char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE];
    assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);
    buffer_pool_manager->unpin_page(PageId{fd, page_no}, false);
}

void check_cache_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_cache(fd, page_no);
        }
    }
}

void rand_buf(int size, char *buf) {
    for (int i = 0; i < size; i++) {
        int rand_ch = rand() & 0xff;
        buf[i] = rand_ch;
    }
}

int rand_fd() {
    assert(mock.size() == MAX_FILES);
    int fd_idx = rand() % MAX_FILES;
    auto it = mock.begin();
    for (int i = 0; i < fd_idx; i++) {
        it++;
    }
    return it->first;
}

struct rid_hash_t {
    size_t operator()(const Rid &rid) const { return (rid.page_no << 16) | rid.slot_no; }
};

struct rid_equal_t {
    bool operator()(const Rid &x, const Rid &y) const { return x.page_no == y.page_no && x.slot_no == y.slot_no; }
};

void check_equal(const RmFileHandle *file_handle,
                 const std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> &mock) {
    // Test all records
    for (auto &entry : mock) {
        Rid rid = entry.first;
        auto mock_buf = (char *)entry.second.c_str();
        auto rec = file_handle->get_record(rid, nullptr);
        assert(memcmp(mock_buf, rec->data, file_handle->file_hdr_.record_size) == 0);
    }
    // Randomly get record
    for (int i = 0; i < 10; i++) {
        Rid rid = {.page_no = 1 + rand() % (file_handle->file_hdr_.num_pages - 1),
                   .slot_no = rand() % file_handle->file_hdr_.num_records_per_page};
        bool mock_exist = mock.count(rid) > 0;
        bool rm_exist = file_handle->is_record(rid);
        assert(rm_exist == mock_exist);
    }
    // Test RM scan
    size_t num_records = 0;
    for (RmScan scan(file_handle); !scan.is_end(); scan.next()) {
        assert(mock.count(scan.rid()) > 0);
        auto rec = file_handle->get_record(scan.rid(), nullptr);
        assert(memcmp(rec->data, mock.at(scan.rid()).c_str(), file_handle->file_hdr_.record_size) == 0);
        num_records++;
    }
    assert(num_records == mock.size());
}

// std::cout can call this, for example: std::cout << rid
std::ostream &operator<<(std::ostream &os, const Rid &rid) {
    return os << '(' << rid.page_no << ", " << rid.slot_no << ')';
}

namespace {

auto make_int_col(const std::string &table_name, const std::string &col_name, int offset) -> ColMeta {
    return ColMeta{.tab_name = table_name,
                   .name = col_name,
                   .type = TYPE_INT,
                   .len = static_cast<int>(sizeof(int)),
                   .offset = offset,
                   .index = false};
}

auto make_int_record(std::initializer_list<int> values) -> RmRecord {
    RmRecord record(static_cast<int>(values.size() * sizeof(int)));
    int offset = 0;
    for (int value : values) {
        memcpy(record.data + offset, &value, sizeof(int));
        offset += static_cast<int>(sizeof(int));
    }
    return record;
}

auto read_int_field(const RmRecord &record, int offset) -> int {
    return *reinterpret_cast<const int *>(record.data + offset);
}

auto parse_sql(const std::string &sql) -> std::shared_ptr<ast::TreeNode> {
    yyscan_t yyscanner;
    if (yylex_init(&yyscanner) != 0) {
        throw std::runtime_error("failed to initialize scanner");
    }
    std::shared_ptr<ast::TreeNode> parse_tree;
    YY_BUFFER_STATE buf = yy_scan_string(sql.c_str(), yyscanner);
    int parse_result = yyparse(&parse_tree, yyscanner);
    yy_delete_buffer(buf, yyscanner);
    yylex_destroy(yyscanner);
    if (parse_result != 0) {
        throw std::runtime_error("failed to parse SQL");
    }
    return parse_tree;
}

auto make_int_table(const std::string &table_name, std::initializer_list<std::string> col_names) -> TabMeta {
    TabMeta table;
    table.name = table_name;
    int offset = 0;
    for (const auto &col_name : col_names) {
        table.cols.push_back(make_int_col(table_name, col_name, offset));
        offset += static_cast<int>(sizeof(int));
    }
    return table;
}

/**
 * Minimal in-memory executor used to unit test join executors without
 * depending on the storage layer or planner pipeline.
 */
class MockExecutor : public AbstractExecutor {
   public:
    MockExecutor(std::vector<ColMeta> cols, std::vector<RmRecord> rows)
        : cols_(std::move(cols)), rows_(std::move(rows)) {
        tuple_len_ = 0;
        for (const auto &col : cols_) {
            tuple_len_ = std::max(tuple_len_, static_cast<size_t>(col.offset + col.len));
        }
    }

    void beginTuple() override { cursor_ = 0; }

    void nextTuple() override {
        if (cursor_ < rows_.size()) {
            ++cursor_;
        }
    }

    bool is_end() const override { return cursor_ >= rows_.size(); }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(rows_[cursor_]);
    }

    Rid &rid() override {
        current_rid_ = Rid{0, static_cast<int>(cursor_)};
        return current_rid_;
    }

    size_t tupleLen() const override { return tuple_len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

   private:
    std::vector<ColMeta> cols_;
    std::vector<RmRecord> rows_;
    size_t tuple_len_ = 0;
    size_t cursor_ = 0;
    Rid current_rid_{-1, -1};
};

auto make_eq_condition(const std::string &left_table, const std::string &left_col, const std::string &right_table,
                       const std::string &right_col) -> Condition {
    Condition cond;
    cond.lhs_col = TabCol{left_table, left_col};
    cond.op = OP_EQ;
    cond.is_rhs_val = false;
    cond.rhs_col = TabCol{right_table, right_col};
    return cond;
}

auto collect_executor_rows(AbstractExecutor &executor) -> std::vector<RmRecord> {
    std::vector<RmRecord> rows;
    for (executor.beginTuple(); !executor.is_end(); executor.nextTuple()) {
        auto record = executor.Next();
        if (record == nullptr) {
            throw std::runtime_error("Executor returned null record before end");
        }
        rows.push_back(*record);
    }
    return rows;
}

}  // namespace

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_BIG，记录其文件描述符fd */

class BigStorageTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_BIG)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_BIG);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_BIG);
        assert(disk_manager_->is_file(TEST_FILE_NAME_BIG));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME_BIG);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME_BIG);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST(LRUReplacerTest, SampleTest) {
    LRUReplacer lru_replacer(7);

    // Scenario: unpin six elements, i.e. add them to the replacer.
    lru_replacer.unpin(1);
    lru_replacer.unpin(2);
    lru_replacer.unpin(3);
    lru_replacer.unpin(4);
    lru_replacer.unpin(5);
    lru_replacer.unpin(6);
    lru_replacer.unpin(1);
    EXPECT_EQ(6, lru_replacer.Size());

    // Scenario: get three victims from the lru.
    int value;
    lru_replacer.victim(&value);
    EXPECT_EQ(1, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(2, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(3, value);

    // Scenario: pin elements in the replacer.
    // Note that 3 has already been victimized, so pinning 3 should have no effect.
    lru_replacer.pin(3);
    lru_replacer.pin(4);
    EXPECT_EQ(2, lru_replacer.Size());

    // Scenario: unpin 4. We expect that the reference bit of 4 will be set to 1.
    lru_replacer.unpin(4);

    // Scenario: continue looking for victims. We expect these victims.
    lru_replacer.victim(&value);
    EXPECT_EQ(5, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(6, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(4, value);
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME，记录其文件描述符fd */
class BufferPoolManagerTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME)) {
            disk_manager_->destroy_file(TEST_FILE_NAME);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME);
        assert(disk_manager_->is_file(TEST_FILE_NAME));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

// NOLINTNEXTLINE
TEST_F(BufferPoolManagerTest, SampleTest) {
    // create BufferPoolManager
    const size_t buffer_pool_size = 10;
    auto disk_manager = BufferPoolManagerTest::disk_manager_.get();
    auto bpm = std::make_unique<BufferPoolManager>(buffer_pool_size, disk_manager);
    // create tmp PageId
    int fd = BufferPoolManagerTest::fd_;
    PageId page_id_temp = {.fd = fd, .page_no = INVALID_PAGE_ID};
    auto *page0 = bpm->new_page(&page_id_temp);

    // Scenario: The buffer pool is empty. We should be able to create a new page.
    ASSERT_NE(nullptr, page0);
    EXPECT_EQ(0, page_id_temp.page_no);

    // Scenario: Once we have a page, we should be able to read and write content.
    snprintf(page0->get_data(), sizeof(page0->get_data()), "Hello");
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));

    // Scenario: We should be able to create new pages until we fill up the buffer pool.
    for (size_t i = 1; i < buffer_pool_size; ++i) {
        EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: Once the buffer pool is full, we should not be able to create any new pages.
    for (size_t i = buffer_pool_size; i < buffer_pool_size * 2; ++i) {
        EXPECT_EQ(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: After unpinning pages {0, 1, 2, 3, 4} and pinning another 4 new pages,
    // there would still be one cache frame left for reading page 0.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(true, bpm->unpin_page(PageId{fd, i}, true));
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: We should be able to fetch the data we wrote a while ago.
    page0 = bpm->fetch_page(PageId{fd, 0});
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));
    EXPECT_EQ(true, bpm->unpin_page(PageId{fd, 0}, true));
    // new_page again, and now all buffers are pinned. Page 0 would be failed to fetch.
    EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    EXPECT_EQ(nullptr, bpm->fetch_page(PageId{fd, 0}));

    bpm->flush_all_pages(fd);
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_CCUR，记录其文件描述符fd */

// Add by jiawen
class BufferPoolManagerConcurrencyTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_CCUR)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_CCUR);
        assert(disk_manager_->is_file(TEST_FILE_NAME_CCUR));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME_CCUR);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST_F(BufferPoolManagerConcurrencyTest, ConcurrencyTest) {
    const int num_threads = 5;
    const int num_runs = 50;

    // get fd
    int fd = BufferPoolManagerConcurrencyTest::fd_;

    for (int run = 0; run < num_runs; run++) {
        // create BufferPoolManager
        auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
        std::shared_ptr<BufferPoolManager> bpm{new BufferPoolManager(50, disk_manager)};

        std::vector<std::thread> threads;
        for (int tid = 0; tid < num_threads; tid++) {
            threads.push_back(std::thread([&bpm, fd]() {  // NOLINT
                PageId temp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
                std::vector<PageId> page_ids;
                for (int i = 0; i < 10; i++) {
                    auto new_page = bpm->new_page(&temp_page_id);
                    EXPECT_NE(nullptr, new_page);
                    ASSERT_NE(nullptr, new_page);
                    strcpy(new_page->get_data(), std::to_string(temp_page_id.page_no).c_str());  // NOLINT
                    page_ids.push_back(temp_page_id);
                }
                for (int i = 0; i < 10; i++) {
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[i], true));
                }
                for (int j = 0; j < 10; j++) {
                    auto page = bpm->fetch_page(page_ids[j]);
                    EXPECT_NE(nullptr, page);
                    ASSERT_NE(nullptr, page);
                    EXPECT_EQ(0, std::strcmp(std::to_string(page_ids[j].page_no).c_str(), (page->get_data())));
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[j], true));
                }
                for (int j = 0; j < 10; j++) {
                    EXPECT_EQ(1, bpm->delete_page(page_ids[j]));
                }
                bpm->flush_all_pages(fd);  // add this test by jiawen
            }));
        }  // end loop tid=[0,num_threads)

        for (int i = 0; i < num_threads; i++) {
            threads[i].join();
        }
    }  // end loop run=[0,num_runs)
}

// TODO: fix detected memory leaks found by Google Test
TEST(StorageTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    /** Test disk_manager */
    std::vector<std::string> filenames(MAX_FILES);  // MAX_FILES=32
    std::unordered_map<int, std::string> fd2name;
    for (size_t i = 0; i < filenames.size(); i++) {
        auto &filename = filenames[i];
        filename = std::to_string(i) + ".txt";
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // open without create
        try {
            disk_manager->open_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }

        disk_manager->create_file(filename);
        assert(disk_manager->is_file(filename));
        try {
            disk_manager->create_file(filename);
            assert(false);
        } catch (const FileExistsError &e) {
        }

        // open file
        int fd = disk_manager->open_file(filename);
        char *tmp = new char[PAGE_SIZE * MAX_PAGES];  // TODO: fix error in detected memory leaks

        mock[fd] = tmp;
        fd2name[fd] = filename;

        disk_manager->set_fd2pageno(fd, 0);  // diskmanager在fd对应的文件中从0开始分配page_no
    }

    /** Test buffer_pool_manager*/
    int num_pages = 0;
    char init_buf[PAGE_SIZE];
    for (auto &fh : mock) {
        int fd = fh.first;
        for (page_id_t i = 0; i < MAX_PAGES; i++) {
            rand_buf(PAGE_SIZE, init_buf);  // 将init_buf填充PAGE_SIZE个字节的随机数据

            PageId tmp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
            Page *page = buffer_pool_manager->new_page(&tmp_page_id);
            int page_no = tmp_page_id.page_no;
            assert(page_no != INVALID_PAGE_ID);
            assert(page_no == i);

            memcpy(page->get_data(), init_buf, PAGE_SIZE);
            buffer_pool_manager->unpin_page(PageId{fd, page_no}, true);

            char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE]
            memcpy(mock_buf, init_buf, PAGE_SIZE);

            num_pages++;

            check_cache(fd, page_no);  // 调用了fetch_page, unpin_page
        }
    }
    check_cache_all();

    assert(num_pages == TEST_BUFFER_POOL_SIZE);

    /** Test flush_all_pages() */
    // Flush and test disk
    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    for (int r = 0; r < 10000; r++) {
        int fd = rand_fd();
        int page_no = rand() % MAX_PAGES;
        // fetch page
        Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
        char *mock_buf = mock_get_page(fd, page_no);
        assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);

        // modify
        rand_buf(PAGE_SIZE, init_buf);
        memcpy(page->get_data(), init_buf, PAGE_SIZE);
        memcpy(mock_buf, init_buf, PAGE_SIZE);

        buffer_pool_manager->unpin_page(page->get_page_id(), true);
        // BufferPool::mark_dirty(page);

        // flush
        if (rand() % 10 == 0) {
            buffer_pool_manager->flush_page(page->get_page_id());
            check_disk(fd, page_no);
        }
        // flush entire file
        if (rand() % 100 == 0) {
            buffer_pool_manager->flush_all_pages(fd);
        }
        // re-open file
        if (rand() % 100 == 0) {
            disk_manager->close_file(fd);
            auto filename = fd2name[fd];
            char *buf = mock[fd];
            fd2name.erase(fd);
            mock.erase(fd);
            int new_fd = disk_manager->open_file(filename);
            mock[new_fd] = buf;
            fd2name[new_fd] = filename;
        }
        // assert equal in cache
        check_cache(fd, page_no);
    }
    check_cache_all();

    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    // close and destroy files
    for (auto &entry : fd2name) {
        int fd = entry.first;
        auto &filename = entry.second;
        disk_manager->close_file(fd);
        disk_manager->destroy_file(filename);
        try {
            disk_manager->destroy_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }
    }
}

TEST(DiskManagerTest, CloseOnlyReleasesFileOnLastReference) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    const std::string filename = "disk_manager_refcount.txt";

    if (local_disk_manager->is_file(filename)) {
        local_disk_manager->destroy_file(filename);
    }
    local_disk_manager->create_file(filename);

    int fd1 = local_disk_manager->open_file(filename);
    int fd2 = local_disk_manager->open_file(filename);
    ASSERT_EQ(fd1, fd2);

    std::array<char, PAGE_SIZE> write_buf{};
    snprintf(write_buf.data(), write_buf.size(), "shared-open");

    local_disk_manager->close_file(fd1);

    EXPECT_NO_THROW(local_disk_manager->write_page(fd2, 0, write_buf.data(), PAGE_SIZE));

    std::array<char, PAGE_SIZE> read_buf{};
    EXPECT_NO_THROW(local_disk_manager->read_page(fd2, 0, read_buf.data(), PAGE_SIZE));
    EXPECT_STREQ("shared-open", read_buf.data());

    local_disk_manager->close_file(fd2);
    local_disk_manager->destroy_file(filename);
}

TEST(DiskManagerTest, DirectoryOperationsSupportWhitespacePaths) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    const std::string dir_path = "__rmdb_dir__ __rmdb_space__ __rmdb_case__";
    const std::string child_path = dir_path + "/child.txt";

    std::filesystem::remove_all(dir_path);
    std::filesystem::remove_all("__rmdb_dir__");
    std::filesystem::remove_all("__rmdb_space__");
    std::filesystem::remove_all("__rmdb_case__");

    local_disk_manager->create_dir(dir_path);
    EXPECT_TRUE(local_disk_manager->is_dir(dir_path));

    local_disk_manager->create_file(child_path);
    EXPECT_TRUE(local_disk_manager->is_file(child_path));

    int fd = local_disk_manager->open_file(child_path);
    local_disk_manager->close_file(fd);

    local_disk_manager->destroy_dir(dir_path);
    EXPECT_FALSE(local_disk_manager->is_dir(dir_path));

    std::filesystem::remove_all("__rmdb_dir__");
    std::filesystem::remove_all("__rmdb_space__");
    std::filesystem::remove_all("__rmdb_case__");
}

TEST(DiskManagerTest, ConcurrentReadsUseStableOffsets) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    const std::string filename = "disk_manager_concurrent_reads.txt";

    if (local_disk_manager->is_file(filename)) {
        local_disk_manager->destroy_file(filename);
    }
    local_disk_manager->create_file(filename);
    int fd = local_disk_manager->open_file(filename);

    std::array<char, PAGE_SIZE> page_zero{};
    std::array<char, PAGE_SIZE> page_one{};
    memset(page_zero.data(), 'A', page_zero.size());
    memset(page_one.data(), 'B', page_one.size());
    local_disk_manager->write_page(fd, 0, page_zero.data(), PAGE_SIZE);
    local_disk_manager->write_page(fd, 1, page_one.data(), PAGE_SIZE);

    std::atomic<bool> corrupted{false};
    auto reader = [&](page_id_t page_no, const std::array<char, PAGE_SIZE> &expected) {
        std::array<char, PAGE_SIZE> buf{};
        for (int i = 0; i < 20000 && !corrupted.load(); ++i) {
            local_disk_manager->read_page(fd, page_no, buf.data(), PAGE_SIZE);
            if (memcmp(buf.data(), expected.data(), PAGE_SIZE) != 0) {
                corrupted.store(true);
                return;
            }
            std::this_thread::yield();
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(reader, 0, std::cref(page_zero));
        threads.emplace_back(reader, 1, std::cref(page_one));
    }
    for (auto &thread : threads) {
        thread.join();
    }

    EXPECT_FALSE(corrupted.load());

    local_disk_manager->close_file(fd);
    local_disk_manager->destroy_file(filename);
}

TEST(BufferPoolManagerRegressionTest, DirtyFlagSurvivesReadonlyUnpin) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(1, local_disk_manager.get());
    const std::string filename = "buffer_pool_dirty_regression.txt";

    if (local_disk_manager->is_file(filename)) {
        local_disk_manager->destroy_file(filename);
    }
    local_disk_manager->create_file(filename);
    int fd = local_disk_manager->open_file(filename);
    local_disk_manager->set_fd2pageno(fd, 0);

    PageId page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
    Page *page = local_buffer_pool_manager->new_page(&page_id);
    ASSERT_NE(nullptr, page);
    snprintf(page->get_data(), PAGE_SIZE, "dirty-page");
    ASSERT_TRUE(local_buffer_pool_manager->unpin_page(page_id, true));

    page = local_buffer_pool_manager->fetch_page(page_id);
    ASSERT_NE(nullptr, page);
    EXPECT_STREQ("dirty-page", page->get_data());
    ASSERT_TRUE(local_buffer_pool_manager->unpin_page(page_id, false));

    PageId other_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
    Page *other_page = local_buffer_pool_manager->new_page(&other_page_id);
    ASSERT_NE(nullptr, other_page);
    ASSERT_TRUE(local_buffer_pool_manager->unpin_page(other_page_id, false));

    page = local_buffer_pool_manager->fetch_page(page_id);
    ASSERT_NE(nullptr, page);
    EXPECT_STREQ("dirty-page", page->get_data());
    ASSERT_TRUE(local_buffer_pool_manager->unpin_page(page_id, false));

    local_disk_manager->close_file(fd);
    local_disk_manager->destroy_file(filename);
}

TEST(BufferPoolManagerRegressionTest, ReusedFdDoesNotHitCachedPageFromClosedFile) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(2, local_disk_manager.get());
    const std::string first_filename = "buffer_pool_fd_reuse_first.txt";
    const std::string second_filename = "buffer_pool_fd_reuse_second.txt";

    if (local_disk_manager->is_file(first_filename)) {
        local_disk_manager->destroy_file(first_filename);
    }
    if (local_disk_manager->is_file(second_filename)) {
        local_disk_manager->destroy_file(second_filename);
    }
    local_disk_manager->create_file(first_filename);
    int first_fd = local_disk_manager->open_file(first_filename);
    local_disk_manager->set_fd2pageno(first_fd, 0);

    PageId first_page_id = {.fd = first_fd, .page_no = INVALID_PAGE_ID};
    Page *page = local_buffer_pool_manager->new_page(&first_page_id);
    ASSERT_NE(nullptr, page);
    snprintf(page->get_data(), PAGE_SIZE, "first-file-page");
    ASSERT_TRUE(local_buffer_pool_manager->unpin_page(first_page_id, true));
    ASSERT_TRUE(local_buffer_pool_manager->flush_page(first_page_id));
    local_disk_manager->fd2pageno_[first_fd] = 7;

    local_disk_manager->close_file(first_fd);

    EXPECT_EQ(0, local_disk_manager->fd2pageno_[first_fd].load());

    local_disk_manager->create_file(second_filename);
    int second_fd = local_disk_manager->open_file(second_filename);
    ASSERT_EQ(first_fd, second_fd);

    std::array<char, PAGE_SIZE> second_page{};
    snprintf(second_page.data(), second_page.size(), "second-file-page");
    local_disk_manager->write_page(second_fd, 0, second_page.data(), PAGE_SIZE);

    Page *second_cached_page = local_buffer_pool_manager->fetch_page(PageId{second_fd, 0});
    ASSERT_NE(nullptr, second_cached_page);
    EXPECT_STREQ("second-file-page", second_cached_page->get_data());
    ASSERT_TRUE(local_buffer_pool_manager->unpin_page(PageId{second_fd, 0}, false));

    local_disk_manager->close_file(second_fd);
    local_disk_manager->destroy_file(first_filename);
    local_disk_manager->destroy_file(second_filename);
}

TEST(IndexManagerRegressionTest, ReopenRestoresNextPageNumber) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(8, local_disk_manager.get());
    auto local_index_manager = std::make_unique<IxManager>(local_disk_manager.get(), local_buffer_pool_manager.get());

    const std::string table_name = "ix_page_allocator_regression";
    std::vector<ColMeta> index_cols = {{
        .tab_name = table_name,
        .name = "id",
        .type = TYPE_INT,
        .len = static_cast<int>(sizeof(int)),
        .offset = 0,
        .index = false,
    }};

    const std::string index_filename = local_index_manager->get_index_name(table_name, index_cols);
    if (local_disk_manager->is_file(index_filename)) {
        local_disk_manager->destroy_file(index_filename);
    }

    local_index_manager->create_index(table_name, index_cols);

    auto index_handle = local_index_manager->open_index(table_name, index_cols);
    auto first_node = index_handle->create_node();
    ASSERT_NE(nullptr, first_node);
    EXPECT_EQ(IX_INIT_NUM_PAGES, first_node->get_page_no());
    ASSERT_TRUE(local_buffer_pool_manager->unpin_page(first_node->get_page_id(), true));
    local_index_manager->close_index(index_handle.get());

    index_handle = local_index_manager->open_index(table_name, index_cols);
    auto second_node = index_handle->create_node();
    ASSERT_NE(nullptr, second_node);
    EXPECT_EQ(IX_INIT_NUM_PAGES + 1, second_node->get_page_no());
    ASSERT_TRUE(local_buffer_pool_manager->unpin_page(second_node->get_page_id(), true));
    local_index_manager->close_index(index_handle.get());

    local_index_manager->destroy_index(table_name, index_cols);
}

class IndexHandleRegressionTest : public ::testing::Test {
   protected:
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<IxManager> index_manager_;
    std::string table_name_;
    std::vector<ColMeta> index_cols_;

    void SetUp() override {
        disk_manager_ = std::make_unique<DiskManager>();
        buffer_pool_manager_ = std::make_unique<BufferPoolManager>(32, disk_manager_.get());
        index_manager_ = std::make_unique<IxManager>(disk_manager_.get(), buffer_pool_manager_.get());
        table_name_ = std::string("ix_handle_regression_") + ::testing::UnitTest::GetInstance()->current_test_info()->name();
        index_cols_ = {{
            .tab_name = table_name_,
            .name = "id",
            .type = TYPE_INT,
            .len = static_cast<int>(sizeof(int)),
            .offset = 0,
            .index = false,
        }};
        destroy_index_if_exists();
    }

    void TearDown() override { destroy_index_if_exists(); }

    void destroy_index_if_exists() {
        const std::string index_name = index_manager_->get_index_name(table_name_, index_cols_);
        if (disk_manager_->is_file(index_name) && !disk_manager_->is_file_open(index_name)) {
            index_manager_->destroy_index(table_name_, index_cols_);
        }
    }

    std::unique_ptr<IxIndexHandle> create_open_index() {
        index_manager_->create_index(table_name_, index_cols_);
        return index_manager_->open_index(table_name_, index_cols_);
    }

    void close_index(std::unique_ptr<IxIndexHandle> &ih) {
        index_manager_->close_index(ih.get());
        ih.reset();
    }

    static void insert_int(IxIndexHandle *ih, int key) {
        ih->insert_entry(reinterpret_cast<const char *>(&key), Rid{key, key + 1000}, nullptr);
    }

    static bool delete_int(IxIndexHandle *ih, int key) {
        return ih->delete_entry(reinterpret_cast<const char *>(&key), Rid{key, key + 1000}, nullptr);
    }

    static void expect_find_int(IxIndexHandle *ih, int key) {
        std::vector<Rid> result;
        ASSERT_TRUE(ih->get_value(reinterpret_cast<const char *>(&key), &result, nullptr));
        ASSERT_EQ(1, result.size());
        EXPECT_EQ(key, result[0].page_no);
        EXPECT_EQ(key + 1000, result[0].slot_no);
    }
};

TEST_F(IndexHandleRegressionTest, BoundsPastLastKeyReturnLeafEnd) {
    auto ih = create_open_index();
    insert_int(ih.get(), 1);

    int high_key = 2;
    Iid end = ih->leaf_end();

    EXPECT_EQ(end, ih->lower_bound(reinterpret_cast<const char *>(&high_key)));
    EXPECT_EQ(end, ih->upper_bound(reinterpret_cast<const char *>(&high_key)));

    close_index(ih);
}

TEST_F(IndexHandleRegressionTest, DeleteLastKeyLeavesIndexReusable) {
    auto ih = create_open_index();
    insert_int(ih.get(), 1);

    ASSERT_TRUE(delete_int(ih.get(), 1));
    insert_int(ih.get(), 2);

    expect_find_int(ih.get(), 2);

    close_index(ih);
}

TEST_F(IndexHandleRegressionTest, RightSiblingRedistributionPreservesMovedKey) {
    auto ih = create_open_index();
    for (int key = 1; key <= 400; ++key) {
        insert_int(ih.get(), key);
    }

    ASSERT_TRUE(delete_int(ih.get(), 1));

    expect_find_int(ih.get(), 170);

    close_index(ih);
}

TEST_F(IndexHandleRegressionTest, RootWithSingleChildIsPromotedAfterCoalesce) {
    auto ih = create_open_index();
    for (int key = 1; key <= 400; ++key) {
        insert_int(ih.get(), key);
    }
    for (int key = 170; key <= 400; ++key) {
        ASSERT_TRUE(delete_int(ih.get(), key));
    }

    auto root = ih->fetch_node(ih->file_hdr_->root_page_);
    EXPECT_TRUE(root->is_leaf_page());
    EXPECT_EQ(IX_NO_PAGE, root->get_parent());
    ASSERT_TRUE(buffer_pool_manager_->unpin_page(root->get_page_id(), false));

    expect_find_int(ih.get(), 1);

    close_index(ih);
}

TEST(RecordManagerTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    // 创建RmManager类的对象rm_manager
    auto disk_manager = std::make_unique<DiskManager>();
    auto buffer_pool_manager = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
    auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());

    std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> mock;

    std::string filename = "abc.txt";

    int record_size = 4 + rand() % 256;  // 元组大小随便设置，只要不超过RM_MAX_RECORD_SIZE
    // test files
    {
        // 删除残留的同名文件
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // 将file header写入到磁盘中的filename文件
        rm_manager->create_file(filename, record_size);
        // 将磁盘中的filename文件读出到内存中的file handle的file header
        std::unique_ptr<RmFileHandle> file_handle = rm_manager->open_file(filename);
        // 检查filename文件在内存中的file header的参数
        assert(file_handle->file_hdr_.record_size == record_size);
        assert(file_handle->file_hdr_.first_free_page_no == RM_NO_PAGE);
        assert(file_handle->file_hdr_.num_pages == 1);

        int max_bytes = file_handle->file_hdr_.record_size * file_handle->file_hdr_.num_records_per_page +
                        file_handle->file_hdr_.bitmap_size + (int)sizeof(RmPageHdr);
        assert(max_bytes <= PAGE_SIZE);
        int rand_val = rand();
        file_handle->file_hdr_.num_pages = rand_val;
        rm_manager->close_file(file_handle.get());

        // reopen file
        file_handle = rm_manager->open_file(filename);
        assert(file_handle->file_hdr_.num_pages == rand_val);
        rm_manager->close_file(file_handle.get());
        rm_manager->destroy_file(filename);
    }
    // test pages
    rm_manager->create_file(filename, record_size);
    auto file_handle = rm_manager->open_file(filename);

    char write_buf[PAGE_SIZE];
    size_t add_cnt = 0;
    size_t upd_cnt = 0;
    size_t del_cnt = 0;
    for (int round = 0; round < 1000; round++) {
        double insert_prob = 1. - mock.size() / 250.;
        double dice = rand() * 1. / RAND_MAX;
        if (mock.empty() || dice < insert_prob) {
            rand_buf(file_handle->file_hdr_.record_size, write_buf);
            Rid rid = file_handle->insert_record(write_buf, nullptr);
            mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
            add_cnt++;
            //            std::cout << "insert " << rid << '\n'; // operator<<(cout,rid)
        } else {
            // update or erase random rid
            int rid_idx = rand() % mock.size();
            auto it = mock.begin();
            for (int i = 0; i < rid_idx; i++) {
                it++;
            }
            auto rid = it->first;
            if (rand() % 2 == 0) {
                // update
                rand_buf(file_handle->file_hdr_.record_size, write_buf);
                file_handle->update_record(rid, write_buf, nullptr);
                mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
                upd_cnt++;
                //                std::cout << "update " << rid << '\n';
            } else {
                // erase
                file_handle->delete_record(rid, nullptr);
                mock.erase(rid);
                del_cnt++;
                //                std::cout << "delete " << rid << '\n';
            }
        }
        // Randomly re-open file
        if (round % 50 == 0) {
            rm_manager->close_file(file_handle.get());
            file_handle = rm_manager->open_file(filename);
        }
        check_equal(file_handle.get(), mock);
    }
    assert(mock.size() == add_cnt - del_cnt);
    std::cout << "insert " << add_cnt << '\n' << "delete " << del_cnt << '\n' << "update " << upd_cnt << '\n';
    // clean up
    rm_manager->close_file(file_handle.get());
    rm_manager->destroy_file(filename);
}

TEST(OrderByParserTest, ParsesMultiKeyDirections) {
    auto parse_tree = parse_sql("select * from t order by a desc, b asc, c;");
    auto select = std::dynamic_pointer_cast<ast::SelectStmt>(parse_tree);
    ASSERT_NE(nullptr, select);
    ASSERT_TRUE(select->has_sort);
    ASSERT_NE(nullptr, select->order);
    ASSERT_EQ(3, select->order->items.size());

    EXPECT_EQ("a", select->order->items[0]->col->col_name);
    EXPECT_EQ(ast::OrderBy_DESC, select->order->items[0]->orderby_dir);
    EXPECT_EQ("b", select->order->items[1]->col->col_name);
    EXPECT_EQ(ast::OrderBy_ASC, select->order->items[1]->orderby_dir);
    EXPECT_EQ("c", select->order->items[2]->col->col_name);
    EXPECT_EQ(ast::OrderBy_DEFAULT, select->order->items[2]->orderby_dir);
}

TEST(AggregateAliasParserTest, ParsesExplicitAggregateAliases) {
    auto parse_tree = parse_sql("select count(*) as cnt, sum(a) as total from t;");
    auto select = std::dynamic_pointer_cast<ast::SelectStmt>(parse_tree);
    ASSERT_NE(nullptr, select);
    ASSERT_EQ(2, select->select_items.size());

    auto count_item = select->select_items[0];
    ASSERT_NE(nullptr, count_item);
    EXPECT_TRUE(count_item->has_alias);
    EXPECT_EQ("cnt", count_item->alias);
    ASSERT_NE(nullptr, std::dynamic_pointer_cast<ast::AggFunc>(count_item->expr));

    auto sum_item = select->select_items[1];
    ASSERT_NE(nullptr, sum_item);
    EXPECT_TRUE(sum_item->has_alias);
    EXPECT_EQ("total", sum_item->alias);
    ASSERT_NE(nullptr, std::dynamic_pointer_cast<ast::AggFunc>(sum_item->expr));
}

TEST(AggregateAliasParserTest, RejectsNonAggregateAliases) {
    EXPECT_THROW(parse_sql("select a as x from t;"), std::runtime_error);
}

TEST(OrderByPlannerTest, PreservesNormalizedSortKeysAcrossAnalyzeAndPlanner) {
    SmManager sm_manager(nullptr, nullptr, nullptr, nullptr);
    sm_manager.db_.SetTabMeta("t", make_int_table("t", {"a", "b", "c"}));

    Analyze analyze(&sm_manager);
    Planner planner(&sm_manager);

    auto query = analyze.do_analyze(parse_sql("select * from t order by b desc, a asc, b asc;"));
    ASSERT_EQ(3, query->order_by_keys.size());
    EXPECT_EQ("b", query->order_by_keys[0].col.col_name);
    EXPECT_TRUE(query->order_by_keys[0].is_desc);
    EXPECT_EQ("a", query->order_by_keys[1].col.col_name);
    EXPECT_FALSE(query->order_by_keys[1].is_desc);
    EXPECT_EQ("b", query->order_by_keys[2].col.col_name);
    EXPECT_FALSE(query->order_by_keys[2].is_desc);

    auto dml_plan = std::dynamic_pointer_cast<DMLPlan>(planner.do_planner(query, nullptr));
    ASSERT_NE(nullptr, dml_plan);
    auto projection = std::dynamic_pointer_cast<ProjectionPlan>(dml_plan->subplan_);
    ASSERT_NE(nullptr, projection);
    auto sort_plan = std::dynamic_pointer_cast<SortPlan>(projection->subplan_);
    ASSERT_NE(nullptr, sort_plan);
    ASSERT_EQ(query->order_by_keys.size(), sort_plan->sort_keys_.size());
    for (size_t i = 0; i < query->order_by_keys.size(); ++i) {
        EXPECT_TRUE(query->order_by_keys[i].equals(sort_plan->sort_keys_[i]));
    }
}

TEST(LimitParserTest, ParsesLimitAndOffset) {
    auto parse_tree = parse_sql("select * from t order by a desc limit 5 offset 2;");
    auto select = std::dynamic_pointer_cast<ast::SelectStmt>(parse_tree);
    ASSERT_NE(nullptr, select);
    ASSERT_TRUE(select->has_limit);
    ASSERT_NE(nullptr, select->limit_clause);
    EXPECT_EQ(5, select->limit_clause->limit);
    EXPECT_EQ(2, select->limit_clause->offset);
}

TEST(LimitPlannerTest, InsertsLimitPlanBeforeProjection) {
    SmManager sm_manager(nullptr, nullptr, nullptr, nullptr);
    sm_manager.db_.SetTabMeta("t", make_int_table("t", {"a", "b", "c"}));

    Analyze analyze(&sm_manager);
    Planner planner(&sm_manager);

    auto query = analyze.do_analyze(parse_sql("select * from t order by a limit 3 offset 1;"));
    ASSERT_TRUE(query->limit_spec.has_value());
    EXPECT_EQ(3U, query->limit_spec->limit);
    EXPECT_EQ(1U, query->limit_spec->offset);

    auto dml_plan = std::dynamic_pointer_cast<DMLPlan>(planner.do_planner(query, nullptr));
    ASSERT_NE(nullptr, dml_plan);
    auto projection = std::dynamic_pointer_cast<ProjectionPlan>(dml_plan->subplan_);
    ASSERT_NE(nullptr, projection);
    auto limit_plan = std::dynamic_pointer_cast<LimitPlan>(projection->subplan_);
    ASSERT_NE(nullptr, limit_plan);
    EXPECT_EQ(3U, limit_plan->limit_spec_.limit);
    EXPECT_EQ(1U, limit_plan->limit_spec_.offset);
    ASSERT_NE(nullptr, std::dynamic_pointer_cast<SortPlan>(limit_plan->subplan_));
}

TEST(AggregateAliasPlannerTest, KeepsInternalAggregateNamesAndSeparateOutputNames) {
    SmManager sm_manager(nullptr, nullptr, nullptr, nullptr);
    sm_manager.db_.SetTabMeta("t", make_int_table("t", {"a", "b"}));

    Analyze analyze(&sm_manager);
    Planner planner(&sm_manager);

    auto query = analyze.do_analyze(parse_sql("select count(*) as cnt, sum(a) as total from t;"));
    ASSERT_EQ(2, query->select_items.size());
    ASSERT_EQ(2, query->output_names.size());
    EXPECT_EQ("count(*)", query->select_items[0].col_name);
    EXPECT_EQ("sum(a)", query->select_items[1].col_name);
    EXPECT_EQ("cnt", query->output_names[0]);
    EXPECT_EQ("total", query->output_names[1]);

    auto dml_plan = std::dynamic_pointer_cast<DMLPlan>(planner.do_planner(query, nullptr));
    ASSERT_NE(nullptr, dml_plan);
    auto projection = std::dynamic_pointer_cast<ProjectionPlan>(dml_plan->subplan_);
    ASSERT_NE(nullptr, projection);
    ASSERT_EQ(2, projection->sel_cols_.size());
    ASSERT_EQ(2, projection->output_names_.size());
    EXPECT_EQ("count(*)", projection->sel_cols_[0].col_name);
    EXPECT_EQ("sum(a)", projection->sel_cols_[1].col_name);
    EXPECT_EQ("cnt", projection->output_names_[0]);
    EXPECT_EQ("total", projection->output_names_[1]);
}

TEST(ValueCoercionTest, WidensIntLiteralForFloatColumnWrites) {
    Value value;
    value.set_int(90);

    Value coerced = coerce_value_to_type(value, TYPE_FLOAT);
    ASSERT_EQ(TYPE_FLOAT, coerced.type);
    EXPECT_FLOAT_EQ(90.0f, coerced.float_val);
    EXPECT_EQ(nullptr, coerced.raw);

    coerced.init_raw(static_cast<int>(sizeof(float)));
    EXPECT_FLOAT_EQ(90.0f, *reinterpret_cast<const float *>(coerced.raw->data));
}

TEST(ValueCoercionTest, RejectsLossyFloatToIntWrites) {
    Value value;
    value.set_float(90.5f);

    EXPECT_THROW(
        {
            auto coerced = coerce_value_to_type(value, TYPE_INT);
            (void)coerced;
        },
        IncompatibleTypeError);
}

TEST(SortExecutorTest, AppliesMultiKeySortSpecsWithMixedDirections) {
    std::vector<ColMeta> cols = {make_int_col("t", "a", 0), make_int_col("t", "b", 4), make_int_col("t", "payload", 8)};
    auto input = std::make_unique<MockExecutor>(
        cols, std::vector<RmRecord>{make_int_record({1, 10, 100}), make_int_record({1, 20, 200}),
                                    make_int_record({0, 15, 300}), make_int_record({1, 20, 150})});

    SortExecutor executor(
        std::move(input),
        std::vector<SortKeySpec>{{TabCol{"t", "a"}, false}, {TabCol{"t", "b"}, true}});

    auto rows = collect_executor_rows(executor);
    ASSERT_EQ(4, rows.size());
    EXPECT_EQ(300, read_int_field(rows[0], 8));
    EXPECT_EQ(200, read_int_field(rows[1], 8));
    EXPECT_EQ(150, read_int_field(rows[2], 8));
    EXPECT_EQ(100, read_int_field(rows[3], 8));
}

TEST(LimitExecutorTest, AppliesOffsetAndRowCount) {
    std::vector<ColMeta> cols = {make_int_col("t", "a", 0), make_int_col("t", "payload", 4)};
    auto input = std::make_unique<MockExecutor>(
        cols, std::vector<RmRecord>{make_int_record({1, 100}), make_int_record({2, 200}),
                                    make_int_record({3, 300}), make_int_record({4, 400})});

    LimitExecutor executor(std::move(input), LimitSpec{2, 1});

    auto rows = collect_executor_rows(executor);
    ASSERT_EQ(2, rows.size());
    EXPECT_EQ(2, read_int_field(rows[0], 0));
    EXPECT_EQ(200, read_int_field(rows[0], 4));
    EXPECT_EQ(3, read_int_field(rows[1], 0));
    EXPECT_EQ(300, read_int_field(rows[1], 4));
}

TEST(HashJoinExecutorTest, InnerJoinMatchesAllTuplesInBucket) {
    std::vector<ColMeta> left_cols = {make_int_col("left", "k1", 0), make_int_col("left", "k2", 4),
                                      make_int_col("left", "payload", 8)};
    std::vector<ColMeta> right_cols = {make_int_col("right", "k1", 0), make_int_col("right", "k2", 4),
                                       make_int_col("right", "payload", 8)};

    auto left = std::make_unique<MockExecutor>(
        left_cols, std::vector<RmRecord>{make_int_record({1, 10, 100}), make_int_record({2, 20, 200}),
                                         make_int_record({1, 30, 300})});
    auto right = std::make_unique<MockExecutor>(
        right_cols, std::vector<RmRecord>{make_int_record({1, 99, 1000}), make_int_record({3, 88, 3000}),
                                          make_int_record({1, 77, 1100})});

    HashJoinExecutor executor(std::move(left), std::move(right),
                              std::vector<Condition>{make_eq_condition("left", "k1", "right", "k1")},
                              std::vector<Condition>{}, INNER_JOIN);

    auto rows = collect_executor_rows(executor);
    ASSERT_EQ(4, rows.size());
    EXPECT_EQ(100, read_int_field(rows[0], 8));
    EXPECT_EQ(1000, read_int_field(rows[0], 20));
    EXPECT_EQ(100, read_int_field(rows[1], 8));
    EXPECT_EQ(1100, read_int_field(rows[1], 20));
    EXPECT_EQ(300, read_int_field(rows[2], 8));
    EXPECT_EQ(1000, read_int_field(rows[2], 20));
    EXPECT_EQ(300, read_int_field(rows[3], 8));
    EXPECT_EQ(1100, read_int_field(rows[3], 20));
}

TEST(HashJoinExecutorTest, MultiKeyJoinRechecksResidualPredicates) {
    std::vector<ColMeta> left_cols = {make_int_col("left", "k1", 0), make_int_col("left", "k2", 4),
                                      make_int_col("left", "payload", 8)};
    std::vector<ColMeta> right_cols = {make_int_col("right", "k1", 0), make_int_col("right", "k2", 4),
                                       make_int_col("right", "payload", 8)};

    Condition residual;
    residual.lhs_col = TabCol{"left", "payload"};
    residual.op = OP_GT;
    residual.is_rhs_val = false;
    residual.rhs_col = TabCol{"right", "payload"};

    auto left = std::make_unique<MockExecutor>(
        left_cols, std::vector<RmRecord>{make_int_record({1, 1, 5}), make_int_record({1, 2, 7}),
                                         make_int_record({2, 1, 4})});
    auto right = std::make_unique<MockExecutor>(
        right_cols, std::vector<RmRecord>{make_int_record({1, 1, 3}), make_int_record({1, 1, 8}),
                                          make_int_record({1, 2, 7}), make_int_record({2, 1, 1})});

    HashJoinExecutor executor(
        std::move(left), std::move(right),
        std::vector<Condition>{make_eq_condition("left", "k1", "right", "k1"),
                               make_eq_condition("left", "k2", "right", "k2")},
        std::vector<Condition>{residual}, INNER_JOIN);

    auto rows = collect_executor_rows(executor);
    ASSERT_EQ(2, rows.size());
    EXPECT_EQ(5, read_int_field(rows[0], 8));
    EXPECT_EQ(3, read_int_field(rows[0], 20));
    EXPECT_EQ(4, read_int_field(rows[1], 8));
    EXPECT_EQ(1, read_int_field(rows[1], 20));
}

TEST(HashJoinExecutorTest, SemiJoinReturnsEachLeftTupleAtMostOnce) {
    std::vector<ColMeta> left_cols = {make_int_col("left", "k1", 0), make_int_col("left", "k2", 4),
                                      make_int_col("left", "payload", 8)};
    std::vector<ColMeta> right_cols = {make_int_col("right", "k1", 0), make_int_col("right", "k2", 4),
                                       make_int_col("right", "payload", 8)};

    auto left = std::make_unique<MockExecutor>(
        left_cols, std::vector<RmRecord>{make_int_record({1, 10, 100}), make_int_record({2, 20, 200}),
                                         make_int_record({1, 30, 300}), make_int_record({4, 40, 400})});
    auto right = std::make_unique<MockExecutor>(
        right_cols, std::vector<RmRecord>{make_int_record({1, 0, 1}), make_int_record({1, 0, 2}),
                                          make_int_record({2, 0, 3})});

    HashJoinExecutor executor(std::move(left), std::move(right),
                              std::vector<Condition>{make_eq_condition("left", "k1", "right", "k1")},
                              std::vector<Condition>{}, SEMI_JOIN);

    auto rows = collect_executor_rows(executor);
    ASSERT_EQ(3, rows.size());
    EXPECT_EQ(100, read_int_field(rows[0], 8));
    EXPECT_EQ(200, read_int_field(rows[1], 8));
    EXPECT_EQ(300, read_int_field(rows[2], 8));
}
