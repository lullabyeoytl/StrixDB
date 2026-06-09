/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/disk_manager.h"

#include <assert.h>    // for assert
#include <errno.h>
#include <string.h>    // for memset
#include <sys/stat.h>  // for stat
#include <unistd.h>    // for lseek

#include "defs.h"

void write_all_at(int fd, const char *data, int num_bytes, off_t offset) {
    int total_written = 0;
    while (total_written < num_bytes) {
        // use pwrite in case of offset moved by other threads
        ssize_t bytes_written = pwrite(fd, data + total_written, num_bytes - total_written, offset + total_written);
        if (bytes_written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw InternalError("DiskManager::write_page Error");
        }
        if (bytes_written == 0) {
            throw InternalError("DiskManager::write_page Error");
        }
        total_written += static_cast<int>(bytes_written);
    }
}

int read_at(int fd, char *data, int num_bytes, off_t offset) {
    int total_read = 0;
    while (total_read < num_bytes) {
        // use pread in case of offset moved by other threads
        ssize_t bytes_read = pread(fd, data + total_read, num_bytes - total_read, offset + total_read);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw InternalError("DiskManager::read_page Error");
        }
        if (bytes_read == 0) {
            break;
        }
        total_read += static_cast<int>(bytes_read);
    }
    return total_read;
}

DiskManager::DiskManager() {
    for (int i = 0; i < MAX_FD; ++i) {
        fd2pageno_[i] = 0;
    }
}

/**
 * @description: 将数据写入文件的指定磁盘页面中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 写入目标页面的page_id
 * @param {char} *offset 要写入磁盘的数据
 * @param {int} num_bytes 要写入磁盘的数据大小
 */
void DiskManager::write_page(int fd, page_id_t page_no, const char *offset, int num_bytes) {
    const off_t page_offset = static_cast<off_t>(page_no) * PAGE_SIZE;
    write_all_at(fd, offset, num_bytes, page_offset);
}

/**
 * @description: 读取文件中指定编号的页面中的部分数据到内存中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 指定的页面编号
 * @param {char} *offset 读取的内容写入到offset中
 * @param {int} num_bytes 读取的数据量大小
 */
void DiskManager::read_page(int fd, page_id_t page_no, char *offset, int num_bytes) {
    // 1.lseek()定位到文件头，通过(fd,page_no)可以定位指定页面及其在磁盘文件中的偏移量
    // 2.调用read()函数
    // 注意read返回值与num_bytes不等时，throw InternalError("DiskManager::read_page Error");
    const off_t page_offset = static_cast<off_t>(page_no) * PAGE_SIZE;
    const int bytes_read = read_at(fd, offset, num_bytes, page_offset);
    if (bytes_read <= num_bytes) {
        memset(offset + bytes_read, 0, num_bytes - bytes_read);
    }
}

/**
 * @description: 分配一个新的页号
 * @return {page_id_t} 分配的新页号
 * @param {int} fd 指定文件的文件句柄
 */
page_id_t DiskManager::allocate_page(int fd) {
    // 简单的自增分配策略，指定文件的页面编号加1
    assert(fd >= 0 && fd < MAX_FD);
    return fd2pageno_[fd]++;
}

void DiskManager::deallocate_page(__attribute__((unused)) page_id_t page_id) {}

bool DiskManager::is_dir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void DiskManager::create_dir(const std::string &path) {
    std::string cmd = "mkdir -p " + path;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

void DiskManager::destroy_dir(const std::string &path) {
    std::string cmd = "rm -r " + path;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 判断指定路径文件是否存在
 * @return {bool} 若指定路径文件存在则返回true
 * @param {string} &path 指定路径文件
 */
bool DiskManager::is_file(const std::string &path) {
    // 用struct stat获取文件信息
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * @description: 用于创建指定路径文件
 * @return {*}
 * @param {string} &path
 */
void DiskManager::create_file(const std::string &path) {
    // 调用open()函数，使用O_CREAT模式
    // 注意不能重复创建相同文件
    if (is_file(path)) {
        throw FileExistsError(path);
    }
    int fd = open(path.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
        throw UnixError();
    }
    close(fd);
}

/**
 * @description: 删除指定路径的文件
 * @param {string} &path 文件所在路径
 */
void DiskManager::destroy_file(const std::string &path) {
    // 调用unlink()函数
    // 注意不能删除未关闭的文件
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    if (path2fd_.count(path)) {
        throw FileNotClosedError(path);
    }
    if (unlink(path.c_str()) < 0) {
        throw UnixError();
    }
}


/**
 * @description: 打开指定路径文件
 * @return {int} 返回打开的文件的文件句柄
 * @param {string} &path 文件所在路径
 */
int DiskManager::open_file(const std::string &path) {
    // 调用open()函数，使用O_RDWR模式
    // 注意不能重复打开相同文件，并且需要更新文件打开列表
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    {
        std::lock_guard<std::mutex> lock(latch_);
        auto it = path2fd_.find(path);
        if (it != path2fd_.end()) {
            fd2ref_count_[it->second]++;
            return it->second;
        }
    }
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        throw UnixError();
    }
    std::lock_guard<std::mutex> lock(latch_);
    path2fd_[path] = fd;
    fd2path_[fd] = path;
    fd2ref_count_[fd] = 1;
    return fd;
}

/**
 * @description:用于关闭指定路径文件
 * @param {int} fd 打开的文件的文件句柄
 */
void DiskManager::close_file(int fd) {
    std::string path;
    bool should_close = false;
    {
        std::lock_guard<std::mutex> lock(latch_);
        auto path_it = fd2path_.find(fd);
        if (path_it == fd2path_.end()) {
            throw FileNotOpenError(fd);
        }

        auto ref_it = fd2ref_count_.find(fd);
        assert(ref_it != fd2ref_count_.end());
        ref_it->second--;
        if (ref_it->second > 0) {
            return;
        }

        path = path_it->second;
        path2fd_.erase(path);
        fd2path_.erase(path_it);
        fd2ref_count_.erase(ref_it);
        fd2pageno_[fd] = 0;
        should_close = true;
    }

    if (should_close && close(fd) < 0) {
        throw UnixError();
    }
}


/**
 * @description: 获得文件的大小
 * @return {int} 文件的大小
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_size(const std::string &file_name) {
    struct stat stat_buf;
    int rc = stat(file_name.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

/**
 * @description: 根据文件句柄获得文件名
 * @return {string} 文件句柄对应文件的文件名
 * @param {int} fd 文件句柄
 */
std::string DiskManager::get_file_name(int fd) {
    std::lock_guard<std::mutex> lock(latch_);
    auto it = fd2path_.find(fd);
    if (it == fd2path_.end()) {
        throw FileNotOpenError(fd);
    }
    return it->second;
}

/**
 * @description:  获得文件名对应的文件句柄
 * @return {int} 文件句柄
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_fd(const std::string &file_name) {
    {
        std::lock_guard<std::mutex> lock(latch_);
        auto it = path2fd_.find(file_name);
        if (it != path2fd_.end()) {
            return it->second;
        }
    }
    return open_file(file_name);
}

bool DiskManager::is_file_open(const std::string &file_name) {
    std::lock_guard<std::mutex> lock(latch_);
    return path2fd_.count(file_name) > 0;
}


/**
 * @description:  读取日志文件内容
 * @return {int} 返回读取的数据量，若为-1说明读取数据的起始位置超过了文件大小
 * @param {char} *log_data 读取内容到log_data中
 * @param {int} size 读取的数据量大小
 * @param {int} offset 读取的内容在文件中的位置
 */
int DiskManager::read_log(char *log_data, int size, int offset) {
    std::lock_guard<std::mutex> log_lock(log_latch_);
    // read log file from the previous end
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }
    int file_size = get_file_size(LOG_FILE_NAME);
    if (offset > file_size) {
        return -1;
    }

    size = std::min(size, file_size - offset);
    if(size == 0) return 0;
    return read_at(log_fd_, log_data, size, offset);
}


/**
 * @description: 写日志内容
 * @param {char} *log_data 要写入的日志内容
 * @param {int} size 要写入的内容大小
 */
void DiskManager::write_log(char *log_data, int size) {
    std::lock_guard<std::mutex> log_lock(log_latch_);
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }

    // write from the file_end
    const int file_size = get_file_size(LOG_FILE_NAME);
    write_all_at(log_fd_, log_data, size, file_size);
}

void DiskManager::sync_log() {
    std::lock_guard<std::mutex> log_lock(log_latch_);
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }

    while (fdatasync(log_fd_) < 0) {
        if (errno == EINTR) {
            continue;
        }
        throw UnixError();
    }
}
