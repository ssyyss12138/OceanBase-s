/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// COPY ... TO 'path' (format csv, header, nullstr 'x') 的 CSV 写出器。
//
// 设计要点：
//   1. 纯流式：行数据先追加到 64KB 的用户态缓冲区，缓冲区满时立刻 write()，
//      内存占用与结果集大小无关，不会把整表攒在内存里。
//   2. 原子发布：数据写到 <path>.tmp.<pid>，close() 先 flush、fsync，
//      再用"不覆盖"的方式发布到目标路径。目标文件已存在（包括并发创建）
//      时失败，且绝不破坏已有文件。
//   3. 不抛异常：所有失败路径返回 -1，并通过 error() 给出诊断信息。

#include "ob_file_format_reader.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

// 部分老内核头文件没有定义该常量；renameat2 的 RENAME_NOREPLACE 标志固定为 1。
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

namespace oceanbase
{
namespace sql
{
namespace file_sql
{
namespace
{

// 用户态写缓冲区大小。大字段（单次超过该大小）会绕开缓冲区直接 write()。
const size_t COPY_WRITE_BUFFER_SIZE = 64 * 1024;

}  // namespace

class CsvCopyWriter : public FileCopyWriter
{
public:
  CsvCopyWriter(const std::string &path, const FileCopyOptions &options)
    : path_(path),
      tmp_path_(),
      options_(options),
      fd_(-1),
      opened_(false),
      error_()
  {
    buffer_.reserve(COPY_WRITE_BUFFER_SIZE);
  }

  virtual ~CsvCopyWriter()
  {
    // 未正常 close() 就被销毁时，按 abort() 语义清理临时文件。
    if (opened_) {
      abort();
    }
  }

  // 打开写出器：
  //   * 目标文件必须不存在，否则返回 -1，error() 含 "file already exists"；
  //   * 用 O_CREAT | O_EXCL 创建 <path>.tmp.<pid>，避免误覆盖同名的临时文件。
  virtual int open()
  {
    int ret = 0;
    if (opened_) {
      set_error("copy writer is already open: " + path_);
      ret = -1;
    } else if (path_.empty()) {
      set_error("copy target path is empty");
      ret = -1;
    } else if (options_.delimiter_ == '\0' || options_.quote_ == '\0'
               || options_.escape_ == '\0') {
      set_error("invalid csv options: delimiter/quote/escape must not be NUL");
      ret = -1;
    } else {
      struct stat st;
      // 用 lstat：即使是悬空软链接也算"目标已存在"，不会被 rename 悄悄替换掉。
      if (0 == ::lstat(path_.c_str(), &st)) {
        set_error("file already exists: " + path_);
        ret = -1;
      } else if (ENOENT != errno) {
        set_error_with_errno("failed to stat copy target " + path_);
        ret = -1;
      } else {
        tmp_path_ = path_ + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
        fd_ = ::open(tmp_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd_ < 0) {
          set_error_with_errno("failed to create temporary file " + tmp_path_);
          tmp_path_.clear();
          ret = -1;
        } else {
          buffer_.clear();
          opened_ = true;
        }
      }
    }
    return ret;
  }

  // 写表头。options_.header_ == false 时本方法直接返回 0 且不写任何字节，
  // 因此调用方既可以自己判断 header_ 跳过，也可以无条件调用本方法，两种都安全。
  virtual int write_header(const std::vector<std::string> &column_names)
  {
    int ret = 0;
    if (!opened_) {
      set_error("write_header failed: copy writer is not open");
      ret = -1;
    } else if (!options_.header_) {
      // 约定：不写表头，直接成功；不产生任何输出。
      ret = 0;
    } else {
      for (size_t i = 0; 0 == ret && i < column_names.size(); ++i) {
        if (i > 0) {
          ret = append_char(options_.delimiter_);
        }
        if (0 == ret) {
          ret = append_field(column_names.at(i));
        }
      }
      if (0 == ret) {
        ret = append_char('\n');
      }
    }
    return ret;
  }

  // 写一行。NULL 字段写 options_.null_str_ 且不加引号（即使其中含有分隔符）；
  // 其余字段按 CSV 规则转义，行尾补 '\n'。
  virtual int write_row(const std::vector<FileCell> &cells)
  {
    int ret = 0;
    if (!opened_) {
      set_error("write_row failed: copy writer is not open");
      ret = -1;
    } else {
      for (size_t i = 0; 0 == ret && i < cells.size(); ++i) {
        if (i > 0) {
          ret = append_char(options_.delimiter_);
        }
        if (0 != ret) {
          break;
        }
        const FileCell &cell = cells.at(i);
        if (cell.is_null_) {
          // nullstr 按原样输出，不加引号、不转义，与 COPY ... nullstr 'x' 语义一致。
          ret = append_raw(options_.null_str_.data(), options_.null_str_.size());
        } else {
          ret = append_field(cell.text_);
        }
      }
      if (0 == ret) {
        ret = append_char('\n');
      }
    }
    return ret;
  }

  // 发布文件：flush -> fsync -> 不覆盖式 rename。失败时清理临时文件并返回 -1。
  virtual int close()
  {
    int ret = 0;
    if (!opened_) {
      set_error("close failed: copy writer is not open");
      ret = -1;
    } else if (0 != (ret = flush_buffer())) {
      // 写盘出错：临时文件里的数据不完整，直接丢弃，避免留下半成品。
      discard_temporary();
    } else if (0 != ::fsync(fd_)) {
      set_error_with_errno("failed to fsync temporary file " + tmp_path_);
      discard_temporary();
      ret = -1;
    } else {
      if (0 != ::close(fd_)) {
        set_error_with_errno("failed to close temporary file " + tmp_path_);
        fd_ = -1;
        discard_temporary();
        ret = -1;
      } else {
        fd_ = -1;
        if (0 != rename_no_replace()) {
          set_error_with_errno("failed to publish copy file " + path_
                               + " (target may have been created concurrently)");
          discard_temporary();
          ret = -1;
        } else {
          // 目录项落盘是尽力而为：失败不影响已经可见的文件内容。
          fsync_parent_dir();
          tmp_path_.clear();
          opened_ = false;
        }
      }
    }
    if (0 != ret) {
      // close 失败后 writer 已不可用（临时文件也已清理），统一置为未打开状态。
      opened_ = false;
    }
    return ret;
  }

  // 幂等清理：关闭 fd 并删除临时文件，可重复调用；目标文件不受影响。
  virtual void abort()
  {
    discard_temporary();
    buffer_.clear();
    opened_ = false;
  }

  virtual const std::string &error() const { return error_; }

private:
  void set_error(const std::string &message) { error_ = message; }

  void set_error_with_errno(const std::string &prefix)
  {
    error_.assign(prefix);
    error_.append(": ");
    error_.append(std::strerror(errno));
  }

  // 关闭 fd 并删除临时文件；多次调用安全。
  void discard_temporary()
  {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    if (!tmp_path_.empty()) {
      ::unlink(tmp_path_.c_str());
      tmp_path_.clear();
    }
  }

  // 把缓冲区内容完整写出。处理 EINTR 与短写。
  int flush_buffer()
  {
    int ret = 0;
    if (0 != write_all(buffer_.data(), buffer_.size())) {
      ret = -1;
    } else {
      buffer_.clear();
    }
    return ret;
  }

  int write_all(const char *data, size_t len)
  {
    size_t written = 0;
    int ret = 0;
    while (0 == ret && written < len) {
      ssize_t n = ::write(fd_, data + written, len - written);
      if (n < 0) {
        if (EINTR == errno) {
          continue;  // 被信号打断，重试
        }
        set_error_with_errno("failed to write temporary file " + tmp_path_);
        ret = -1;
      } else if (0 == n) {
        set_error("failed to write temporary file " + tmp_path_ + ": write returned 0");
        ret = -1;
      } else {
        written += static_cast<size_t>(n);
      }
    }
    return ret;
  }

  // 追加任意字节：超过缓冲区则先 flush；超大块直接写盘，避免二次拷贝。
  int append_raw(const char *data, size_t len)
  {
    int ret = 0;
    if (len >= COPY_WRITE_BUFFER_SIZE) {
      if (0 == (ret = flush_buffer())) {
        ret = write_all(data, len);
      }
    } else {
      if (buffer_.size() + len > COPY_WRITE_BUFFER_SIZE) {
        ret = flush_buffer();
      }
      if (0 == ret) {
        buffer_.append(data, len);
      }
    }
    return ret;
  }

  int append_char(char c) { return append_raw(&c, 1); }

  // 是否需要整体加引号：含分隔符、引号、换行、回车。
  // 当 escape_ 与 quote_ 不同（例如反斜杠转义）时，escape_ 本身也要触发加引号，
  // 否则该字符会被读写双方当成转义前缀而产生歧义。
  bool need_quote(const std::string &value) const
  {
    return std::string::npos != value.find(options_.delimiter_)
           || std::string::npos != value.find(options_.quote_)
           || std::string::npos != value.find('\n')
           || std::string::npos != value.find('\r')
           || (options_.escape_ != options_.quote_
               && std::string::npos != value.find(options_.escape_));
  }

  // 按 CSV 规则输出一个字段。需要时整体加引号，内部的引号写成
  // escape_ + quote_（escape_ 默认等于 quote_，也就是 "" 风格）。
  int append_field(const std::string &value)
  {
    int ret = 0;
    if (!need_quote(value)) {
      ret = append_raw(value.data(), value.size());
    } else {
      if (0 == (ret = append_char(options_.quote_))) {
        size_t start = 0;
        for (size_t i = 0; 0 == ret && i < value.size(); ++i) {
          const char c = value[i];
          const bool escape_quote = (c == options_.quote_);
          const bool escape_escape =
              (options_.escape_ != options_.quote_ && c == options_.escape_);
          if (!escape_quote && !escape_escape) {
            continue;
          }
          // 先写出这一段普通字符，再写转义序列。
          if (i > start) {
            ret = append_raw(value.data() + start, i - start);
          }
          if (0 == ret) {
            ret = append_char(options_.escape_);
          }
          if (0 == ret) {
            ret = append_char(c);
          }
          start = i + 1;
        }
        if (0 == ret && value.size() > start) {
          ret = append_raw(value.data() + start, value.size() - start);
        }
        if (0 == ret) {
          ret = append_char(options_.quote_);
        }
      }
    }
    return ret;
  }

  // 原子发布，且不允许覆盖已存在的目标文件：
  //   1) Linux 优先用 renameat2(RENAME_NOREPLACE)；
  //   2) 内核/文件系统不支持时退化为 link() + unlink()（link 在目标存在时返回 EEXIST）；
  //   3) 极少数不支持硬链接的文件系统，最后用 stat + rename 兜底。
  int rename_no_replace()
  {
    int ret = -1;
    int saved_errno = 0;
#if defined(__linux__) && defined(SYS_renameat2)
    if (0 == ::syscall(SYS_renameat2, AT_FDCWD, tmp_path_.c_str(),
                       AT_FDCWD, path_.c_str(), RENAME_NOREPLACE)) {
      ret = 0;
    } else {
      saved_errno = errno;
      // ENOSYS/EINVAL 表示内核或文件系统不认识该标志，继续走回退路径。
      if (ENOSYS != saved_errno && EINVAL != saved_errno
          && ENOTSUP != saved_errno && EOPNOTSUPP != saved_errno) {
        errno = saved_errno;
        ret = -1;
      }
    }
#else
    saved_errno = ENOSYS;
#endif

    if (0 != ret) {
      if (0 == ::link(tmp_path_.c_str(), path_.c_str())) {
        ::unlink(tmp_path_.c_str());  // 目标链接已建立，删除临时名
        ret = 0;
      } else if (EEXIST == errno) {
        ret = -1;  // 目标已被并发创建
      } else {
        // link 不被支持时兜底：先检查再 rename（存在竞争窗口，仅用于特殊文件系统）。
        struct stat st;
        if (0 == ::lstat(path_.c_str(), &st)) {
          errno = EEXIST;
          ret = -1;
        } else if (0 != ::rename(tmp_path_.c_str(), path_.c_str())) {
          ret = -1;
        } else {
          ret = 0;
        }
      }
    }
    return ret;
  }

  // 对父目录 fsync，确保 rename 结果持久化；失败可忽略（不影响文件可见性）。
  void fsync_parent_dir() const
  {
    const std::string::size_type pos = path_.find_last_of('/');
    std::string dir;
    if (std::string::npos == pos) {
      dir = ".";
    } else if (0 == pos) {
      dir = "/";
    } else {
      dir = path_.substr(0, pos);
    }
    int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
      ::fsync(dir_fd);
      ::close(dir_fd);
    }
  }

private:
  std::string path_;
  std::string tmp_path_;
  FileCopyOptions options_;
  int fd_;
  bool opened_;
  std::string error_;
  std::string buffer_;
};

int create_file_copy_writer(const std::string &path,
                            const FileCopyOptions &options,
                            std::unique_ptr<FileCopyWriter> &writer)
{
  int ret = 0;
  writer.reset();
  if (path.empty()) {
    // 没有 writer 实例可用于携带错误信息，只能返回失败；SQL 层会保证路径非空。
    ret = -1;
  } else {
    CsvCopyWriter *impl = new (std::nothrow) CsvCopyWriter(path, options);
    if (NULL == impl) {
      ret = -1;
    } else {
      writer.reset(impl);
    }
  }
  return ret;
}

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
