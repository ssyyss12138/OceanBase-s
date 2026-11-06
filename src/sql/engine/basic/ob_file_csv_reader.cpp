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

// ============================================================================
// 自包含的 CSV 读取器 + 确定性类型推断实现。
//
// 设计要点：
//   1. 只使用 C++17 标准库 + POSIX（opendir/readdir/stat），不引用项目内其它头文件，
//      因此可以直接单文件编译、单测；
//   2. 不抛异常：所有错误通过返回码 + error() 文本上报；
//   3. 流式读取：内部固定 64KB 缓冲区逐块读取，绝不把整个文件读进内存；
//      open() 为了做全量类型推断会完整扫一遍文件，扫描结束后 fseek 回到数据起点；
//   4. 类型推断是确定性的：每个值先独立判定类型，再逐列做"加宽"（widen）。
// ============================================================================

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "ob_file_format_reader.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

namespace
{

const int64_t CSV_READ_BUFFER_SIZE = 64 * 1024;  // 64KB 流式读缓冲

inline bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

inline char to_lower_ascii(char c)
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// 读取 "pos 开始、长度 len" 的一段十进制数字，任何一位不是数字都失败。
bool read_fixed_uint(const std::string &text, size_t pos, size_t len, int &value)
{
  if (pos + len > text.size()) {
    return false;
  }
  int v = 0;
  for (size_t i = 0; i < len; ++i) {
    char c = text[pos + i];
    if (!is_ascii_digit(c)) {
      return false;
    }
    v = v * 10 + (c - '0');
  }
  value = v;
  return true;
}

bool is_leap_year(int year)
{
  return (0 == year % 4 && 0 != year % 100) || 0 == year % 400;
}

// 返回某年某月的天数，月份非法时返回 0。
int month_days(int year, int month)
{
  static const int DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (2 == month && is_leap_year(year)) {
    return 29;
  }
  return DAYS[month - 1];
}

// 合法的年月日：月份 1-12，日期在该月范围内（自动处理闰年）。
bool is_valid_ymd(int year, int month, int day)
{
  const int days = (year >= 1 && year <= 9999) ? month_days(year, month) : 0;
  return days > 0 && day >= 1 && day <= days;
}

// 浮点文本语法检查（纯词法分析，不做实际数值转换，避免依赖 locale）。
// 接受：可选 +/- 号 + 十进制尾数（数字与至多一个小数点）+ 可选指数。
// 不接受：十六进制、inf/nan、千分位、首尾空白、空串。
bool is_double_syntax(const std::string &text)
{
  const size_t n = text.size();
  size_t i = 0;
  if (0 == n) {
    return false;
  }
  if ('+' == text[i] || '-' == text[i]) {
    ++i;
  }
  size_t mantissa_digits = 0;
  while (i < n && is_ascii_digit(text[i])) {
    ++i;
    ++mantissa_digits;
  }
  if (i < n && '.' == text[i]) {
    ++i;
    while (i < n && is_ascii_digit(text[i])) {
      ++i;
      ++mantissa_digits;
    }
  }
  if (0 == mantissa_digits) {
    return false;
  }
  if (i < n && ('e' == text[i] || 'E' == text[i])) {
    ++i;
    if (i < n && ('+' == text[i] || '-' == text[i])) {
      ++i;
    }
    size_t exp_digits = 0;
    while (i < n && is_ascii_digit(text[i])) {
      ++i;
      ++exp_digits;
    }
    if (0 == exp_digits) {
      return false;
    }
  }
  return i == n;
}

// 路径拼接：避免 dir 以 '/' 结尾（尤其是根目录 "/"）时产生 "//name"。
std::string join_dir_name(const std::string &dir, const std::string &name)
{
  if (dir.empty()) {
    return name;
  }
  if ('/' == dir[dir.size() - 1]) {
    return dir + name;
  }
  return dir + "/" + name;
}

// 单层 shell 通配符匹配：'*' 匹配任意长度（不含 '/'）的字符序列，
// '?' 匹配恰好一个（非 '/'）字符，其余字符按字面量比较。
bool wildcard_match(const std::string &pattern, const std::string &name)
{
  const size_t np = pattern.size();
  const size_t nn = name.size();
  size_t p = 0;
  size_t n = 0;
  size_t star_p = std::string::npos;  // 最近一次 '*' 在 pattern 中的位置
  size_t star_n = 0;                  // 该 '*' 当前"吞掉"到 name 的哪个位置
  while (n < nn) {
    if (p < np && ('?' == pattern[p] || pattern[p] == name[n])) {
      if ('?' == pattern[p] && '/' == name[n]) {
        return false;  // '?' 不跨目录分隔符
      }
      ++p;
      ++n;
    } else if (p < np && '*' == pattern[p]) {
      star_p = p;
      star_n = n;
      ++p;
    } else if (std::string::npos != star_p) {
      if ('/' == name[star_n]) {
        return false;  // '*' 同样不跨目录分隔符
      }
      p = star_p + 1;
      n = ++star_n;
    } else {
      return false;
    }
  }
  while (p < np && '*' == pattern[p]) {
    ++p;
  }
  return p == np;
}

}  // namespace

// ============================================================================
// 确定性类型推断辅助函数
// ============================================================================

// BIGINT 文本规则：可选正负号、十进制数字，且值必须完整落在 int64 范围。
// 手工累加避免依赖 locale，并正确接受 INT64_MIN。
bool file_text_is_bigint(const std::string &text)
{
  const size_t n = text.size();
  if (0 == n) {
    return false;
  }
  size_t i = 0;
  bool negative = false;
  if ('+' == text[i] || '-' == text[i]) {
    negative = '-' == text[i];
    ++i;
  }
  if (i >= n) {
    return false;
  }
  const uint64_t limit = negative
      ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U
      : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  uint64_t value = 0;
  for (; i < n; ++i) {
    if (!is_ascii_digit(text[i])) {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
    if (value > (limit - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  return true;
}

bool file_text_is_double(const std::string &text)
{
  bool valid = is_double_syntax(text);
  if (valid) {
    errno = 0;
    char *end = NULL;
    const double value = ::strtod(text.c_str(), &end);
    valid = 0 == errno && NULL != end && '\0' == *end && std::isfinite(value);
  }
  return valid;
}

// BOOLEAN 只认 true / false（大小写不敏感）；"1"/"0" 属于 BIGINT，不做隐式转换。
bool file_text_is_boolean(const std::string &text)
{
  if (4 != text.size() && 5 != text.size()) {
    return false;
  }
  std::string lower;
  lower.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    lower.push_back(to_lower_ascii(text[i]));
  }
  return "true" == lower || "false" == lower;
}

// DATE：严格的 YYYY-MM-DD，且必须是历法上真实存在的日期（2026-02-30 非法）。
bool file_text_is_date(const std::string &text)
{
  if (10 != text.size() || '-' != text[4] || '-' != text[7]) {
    return false;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  if (!read_fixed_uint(text, 0, 4, year) ||
      !read_fixed_uint(text, 5, 2, month) ||
      !read_fixed_uint(text, 8, 2, day)) {
    return false;
  }
  return is_valid_ymd(year, month, day);
}

// DATETIME：YYYY-MM-DD[ T]HH:MM:SS，日期合法且 时 00-23 / 分 00-59 / 秒 00-59。
bool file_text_is_datetime(const std::string &text)
{
  if (19 != text.size() || ':' != text[13] || ':' != text[16]) {
    return false;
  }
  if (' ' != text[10] && 'T' != text[10] && 't' != text[10]) {
    return false;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!read_fixed_uint(text, 0, 4, year) ||
      !read_fixed_uint(text, 5, 2, month) ||
      !read_fixed_uint(text, 8, 2, day) ||
      !read_fixed_uint(text, 11, 2, hour) ||
      !read_fixed_uint(text, 14, 2, minute) ||
      !read_fixed_uint(text, 17, 2, second)) {
    return false;
  }
  return is_valid_ymd(year, month, day) && hour <= 23 && minute <= 59 && second <= 59;
}

// 单个单元格的类型推断。空串代表 NULL。
// 判定顺序：BOOLEAN -> BIGINT -> DOUBLE -> DATE -> DATETIME -> VARCHAR，
// 由于各类型的词法集合互斥（日期含 '-'，时间含空格/':'），顺序不影响结果。
FileLogicalType file_infer_type(const std::string &text)
{
  FileLogicalType type = FILE_TYPE_VARCHAR;
  if (text.empty()) {
    type = FILE_TYPE_NULL;
  } else if (file_text_is_boolean(text)) {
    type = FILE_TYPE_BOOLEAN;
  } else if (file_text_is_bigint(text)) {
    type = FILE_TYPE_BIGINT;
  } else if (file_text_is_double(text)) {
    type = FILE_TYPE_DOUBLE;
  } else if (file_text_is_date(text)) {
    type = FILE_TYPE_DATE;
  } else if (file_text_is_datetime(text)) {
    type = FILE_TYPE_DATETIME;
  }
  return type;
}

// 加宽规则（"取最宽类型"）：
//   NULL + T                => T
//   T + T                   => T
//   VARCHAR + 任何类型       => VARCHAR
//   BIGINT + DOUBLE         => DOUBLE
//   其它组合（BOOLEAN/DATE/DATETIME 与别的类型混用）=> VARCHAR
// 最后一条刻意不做隐式转换：不让 true 变成 1，也不让日期变成数值。
FileLogicalType file_widen_type(FileLogicalType left, FileLogicalType right)
{
  if (left == right) {
    return left;
  }
  if (FILE_TYPE_NULL == left) {
    return right;
  }
  if (FILE_TYPE_NULL == right) {
    return left;
  }
  if (FILE_TYPE_VARCHAR == left || FILE_TYPE_VARCHAR == right) {
    return FILE_TYPE_VARCHAR;
  }
  if ((FILE_TYPE_BIGINT == left && FILE_TYPE_DOUBLE == right) ||
      (FILE_TYPE_DOUBLE == left && FILE_TYPE_BIGINT == right)) {
    return FILE_TYPE_DOUBLE;
  }
  return FILE_TYPE_VARCHAR;
}

// 名字与 SQL 层 DESCRIBE 输出保持一致（BIGINT / DOUBLE / DATE ... 大写）。
const char *file_logical_type_name(FileLogicalType type)
{
  const char *name = "UNKNOWN";
  switch (type) {
    case FILE_TYPE_NULL:
      name = "NULL";
      break;
    case FILE_TYPE_BOOLEAN:
      name = "BOOLEAN";
      break;
    case FILE_TYPE_BIGINT:
      name = "BIGINT";
      break;
    case FILE_TYPE_DOUBLE:
      name = "DOUBLE";
      break;
    case FILE_TYPE_DATE:
      name = "DATE";
      break;
    case FILE_TYPE_DATETIME:
      name = "DATETIME";
      break;
    case FILE_TYPE_VARCHAR:
      name = "VARCHAR";
      break;
    default:
      break;
  }
  return name;
}

// ============================================================================
// glob()
// ============================================================================

// 只支持"字面量目录 + 单层通配文件名"的形式：
//   /tmp/dir/*.csv   -> 目录 "/tmp/dir"，文件名模式 "*.csv"
//   data/*           -> 目录 "data"
//   *.csv            -> 目录 "."（当前目录）
// 返回值恒为 0；目录不可读、无匹配都只是返回空列表，不视为错误。
int glob_files(const std::string &pattern, std::vector<std::string> &files)
{
  files.clear();
  if (pattern.empty()) {
    return 0;
  }

  std::string dir;
  std::string name_pattern;
  const size_t slash = pattern.find_last_of('/');
  if (std::string::npos == slash) {
    dir = ".";
    name_pattern = pattern;
  } else {
    dir = pattern.substr(0, slash);
    name_pattern = pattern.substr(slash + 1);
    if (dir.empty()) {
      dir = "/";  // "/xxx" 形式
    }
  }
  if (name_pattern.empty()) {
    return 0;  // 形如 "dir/" 的模式没有文件名部分，视为无匹配
  }

  DIR *dir_handle = ::opendir(dir.c_str());
  if (NULL == dir_handle) {
    return 0;  // 目录不存在或不可读：无匹配
  }
  struct dirent *entry = NULL;
  while (NULL != (entry = ::readdir(dir_handle))) {
    const char *name = entry->d_name;
    if (0 == ::strcmp(name, ".") || 0 == ::strcmp(name, "..")) {
      continue;
    }
    // Match ordinary shell glob semantics: '*' does not discover hidden
    // directory entries unless the filename pattern explicitly starts with
    // '.'. This also prevents macOS AppleDouble sidecars (._*) from being
    // exposed as user data files after fixture deployment.
    if ('.' == name[0] && (name_pattern.empty() || '.' != name_pattern[0])) {
      continue;
    }
    if (!wildcard_match(name_pattern, name)) {
      continue;
    }
    const std::string full_path = join_dir_name(dir, name);
    struct stat st;
    ::memset(&st, 0, sizeof(st));
    if (0 != ::stat(full_path.c_str(), &st)) {
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      continue;  // 只要普通文件，目录/符号链接指向的目录等全部跳过
    }
    files.push_back(full_path);
  }
  ::closedir(dir_handle);

  // 同一目录下按完整路径排序等价于按文件名排序。
  std::sort(files.begin(), files.end());
  return 0;
}

// ============================================================================
// CsvFileReader
// ============================================================================

class CsvFileReader : public FileFormatReader
{
public:
  CsvFileReader();
  CsvFileReader(const std::string &path, const FileReadOptions &options);
  virtual ~CsvFileReader();

  // 允许先构造、后设置输入（便于放入容器 / 复用对象）。
  int set_input(const std::string &path, const FileReadOptions &options);

  virtual int open();
  virtual const std::vector<FileColumnSchema> &get_schema() const { return schema_; }
  virtual int next_batch(int64_t max_rows, std::vector<FileRow> &rows, bool &eof);
  virtual void close();
  virtual const std::string &error() const { return error_; }

private:
  enum
  {
    ROW_OK = 0,   // 成功解析一行
    ROW_EOF = 1,  // 文件结束
    ROW_ERR = -1, // 解析错误
  };

  int fill_buffer();
  int peek_char();
  int next_char();
  // 读一条记录（引号字段可以跨物理行）。返回 ROW_OK / ROW_EOF / ROW_ERR。
  int parse_row(std::vector<FileCell> &cells, bool &eof);
  void append_field(std::vector<FileCell> &cells, const std::string &text);
  int skip_bom();
  int read_header();
  int infer_scan(std::vector<FileLogicalType> &types, std::vector<bool> &nullable);
  void build_schema(const std::vector<FileLogicalType> &types,
                    const std::vector<bool> &nullable);
  int rewind_to_data_start();
  void set_error(const char *fmt, ...);
  std::string make_column_name(int64_t index) const;

  std::string path_;
  FileReadOptions options_;
  std::vector<char> buffer_;  // 64KB 流式缓冲
  size_t buf_pos_;
  size_t buf_len_;
  FILE *fp_;
  bool opened_;
  bool eof_;
  bool read_error_;      // 底层 fread 是否失败（避免把 IO 错误当成文件结束）
  int64_t line_no_;      // 当前物理行号（1 基），用于坏行诊断
  int64_t row_line_;     // 当前记录起始物理行号
  int64_t field_count_;  // 期望字段数；-1 表示尚未确定
  std::vector<std::string> column_names_;
  std::vector<FileColumnSchema> schema_;
  std::string error_;
};

CsvFileReader::CsvFileReader()
  : path_(),
    options_(),
    buffer_(),
    buf_pos_(0),
    buf_len_(0),
    fp_(NULL),
    opened_(false),
    eof_(false),
    read_error_(false),
    line_no_(1),
    row_line_(1),
    field_count_(-1),
    column_names_(),
    schema_(),
    error_()
{
  buffer_.resize(static_cast<size_t>(CSV_READ_BUFFER_SIZE), '\0');
}

CsvFileReader::CsvFileReader(const std::string &path, const FileReadOptions &options)
  : path_(path),
    options_(options),
    buffer_(),
    buf_pos_(0),
    buf_len_(0),
    fp_(NULL),
    opened_(false),
    eof_(false),
    read_error_(false),
    line_no_(1),
    row_line_(1),
    field_count_(-1),
    column_names_(),
    schema_(),
    error_()
{
  buffer_.resize(static_cast<size_t>(CSV_READ_BUFFER_SIZE), '\0');
}

CsvFileReader::~CsvFileReader()
{
  close();
}

int CsvFileReader::set_input(const std::string &path, const FileReadOptions &options)
{
  close();
  path_ = path;
  options_ = options;
  error_.clear();
  return 0;
}

void CsvFileReader::set_error(const char *fmt, ...)
{
  char text[512];
  va_list args;
  va_start(args, fmt);
  ::vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  text[sizeof(text) - 1] = '\0';
  error_.assign(text);
}

std::string CsvFileReader::make_column_name(int64_t index) const
{
  char name[64];
  ::snprintf(name, sizeof(name), "column%lld", static_cast<long long>(index));
  return std::string(name);
}

// 从磁盘补充缓冲区。返回 0 = 有数据，1 = 文件结束，-1 = 读错误。
int CsvFileReader::fill_buffer()
{
  if (buf_pos_ < buf_len_) {
    return 0;
  }
  if (NULL == fp_) {
    return 1;
  }
  const size_t n = ::fread(&buffer_[0], 1, buffer_.size(), fp_);
  if (0 == n) {
    if (0 != ::ferror(fp_)) {
      read_error_ = true;
      return -1;
    }
    return 1;
  }
  buf_pos_ = 0;
  buf_len_ = n;
  return 0;
}

// 查看下一个字节但不消费；文件结束返回 -1。
int CsvFileReader::peek_char()
{
  const int rc = fill_buffer();
  if (0 != rc) {
    return -1;
  }
  return static_cast<unsigned char>(buffer_[buf_pos_]);
}

// 消费一个字节；文件结束返回 -1。换行符在这里统一计数。
int CsvFileReader::next_char()
{
  const int rc = fill_buffer();
  if (0 != rc) {
    return -1;
  }
  const char c = buffer_[buf_pos_++];
  if ('\n' == c) {
    ++line_no_;
  }
  return static_cast<unsigned char>(c);
}

void CsvFileReader::append_field(std::vector<FileCell> &cells, const std::string &text)
{
  cells.push_back(FileCell());
  FileCell &cell = cells.back();
  cell.text_ = text;
  // 空字段即 NULL；配置了 nullstr 时文本完全匹配也算 NULL。
  cell.is_null_ = text.empty() ||
                  (!options_.null_str_.empty() && text == options_.null_str_);
}

// 解析一条 CSV 记录（RFC4180 风格）。
//   * 支持引号字段、字段内逗号/换行、双引号转义（""）；
//   * 行结束符支持 "\n" 与 "\r\n"（单独的 "\r" 也当作行结束）；
//   * 完全空白的物理行会被跳过（文件尾部常见），不参与字段数校验。
int CsvFileReader::parse_row(std::vector<FileCell> &cells, bool &eof)
{
  eof = false;
  cells.clear();

  // 跳过空白行
  for (;;) {
    const int c = peek_char();
    if (c < 0) {
      if (read_error_) {
        set_error("failed to read file '%s': %s", path_.c_str(), ::strerror(errno));
        return ROW_ERR;
      }
      eof = true;
      return ROW_EOF;
    }
    if ('\n' == c) {
      next_char();
      continue;
    }
    if ('\r' == c) {
      next_char();
      const int d = peek_char();
      if ('\n' == d) {
        next_char();
      } else if (d < 0) {
        eof = true;
        return ROW_EOF;
      } else {
        ++line_no_;  // 单独的 CR 也算一次换行
      }
      continue;
    }
    break;
  }
  row_line_ = line_no_;

  const int delimiter_c = static_cast<unsigned char>(
      options_.delimiter_ ? options_.delimiter_ : ',');
  const int quote_c = options_.quote_ ? static_cast<unsigned char>(options_.quote_) : -1;
  const int escape_c = (0 != options_.escape_ && options_.escape_ != options_.quote_)
                           ? static_cast<unsigned char>(options_.escape_)
                           : -1;

  std::string field;
  bool in_quotes = false;
  bool row_done = false;
  while (!row_done) {
    const int c = next_char();
    if (c < 0) {
      if (read_error_) {
        set_error("failed to read file '%s': %s", path_.c_str(), ::strerror(errno));
        return ROW_ERR;
      }
      if (in_quotes) {
        set_error("line %lld: unterminated quoted field",
                  static_cast<long long>(row_line_));
        return ROW_ERR;
      }
      append_field(cells, field);  // 文件末尾没有换行符
      row_done = true;
    } else if (in_quotes) {
      if (c == quote_c) {
        const int d = peek_char();
        if (d == quote_c) {
          next_char();  // "" -> 一个双引号
          field.push_back(static_cast<char>(quote_c));
        } else {
          in_quotes = false;
        }
      } else if (c == escape_c) {
        const int d = next_char();  // 自定义转义符：转义符后的字符按字面量处理
        if (d >= 0) {
          field.push_back(static_cast<char>(d));
        }
      } else {
        field.push_back(static_cast<char>(c));
      }
    } else {
      if (c == delimiter_c) {
        append_field(cells, field);
        field.clear();
      } else if ('\r' == c) {
        const int d = peek_char();
        if ('\n' == d) {
          next_char();
        } else if (d < 0) {
          // 文件以 CR 结束
        } else {
          ++line_no_;
        }
        append_field(cells, field);
        row_done = true;
      } else if ('\n' == c) {
        append_field(cells, field);
        row_done = true;
      } else if (c == quote_c && field.empty()) {
        in_quotes = true;  // 只有字段开头才把引号当作引用开始
      } else {
        field.push_back(static_cast<char>(c));
      }
    }
  }
  return ROW_OK;
}

// 跳过 UTF-8 BOM（EF BB BF）。
int CsvFileReader::skip_bom()
{
  const int rc = fill_buffer();
  if (rc < 0) {
    set_error("failed to read file '%s': %s", path_.c_str(), ::strerror(errno));
    return -1;
  }
  if (0 == rc && buf_len_ - buf_pos_ >= 3) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(&buffer_[buf_pos_]);
    if (0xEF == p[0] && 0xBB == p[1] && 0xBF == p[2]) {
      buf_pos_ += 3;
    }
  }
  return 0;
}

// 读取表头：列名保持原样（重复列名不去重，去重由上层 SQL 层负责）。
int CsvFileReader::read_header()
{
  std::vector<FileCell> cells;
  bool row_eof = false;
  const int rc = parse_row(cells, row_eof);
  if (ROW_ERR == rc) {
    return -1;
  }
  if (ROW_EOF == rc) {
    field_count_ = -1;  // 空文件：没有表头也没有数据
    column_names_.clear();
    return 0;
  }
  field_count_ = static_cast<int64_t>(cells.size());
  column_names_.clear();
  column_names_.reserve(cells.size());
  for (size_t i = 0; i < cells.size(); ++i) {
    column_names_.push_back(cells[i].text_);
  }
  return 0;
}

// 全量扫描：对每一列的所有非 NULL 值做推断再逐步加宽。
// 坏行（字段数不符）不参与推断，留给 next_batch 扫描到该行时统一报错。
int CsvFileReader::infer_scan(std::vector<FileLogicalType> &types,
                              std::vector<bool> &nullable)
{
  if (field_count_ >= 0) {
    types.assign(static_cast<size_t>(field_count_), FILE_TYPE_NULL);
    nullable.assign(static_cast<size_t>(field_count_), false);
  } else {
    types.clear();
    nullable.clear();
  }

  std::vector<FileCell> cells;
  for (;;) {
    bool row_eof = false;
    const int rc = parse_row(cells, row_eof);
    if (ROW_ERR == rc) {
      return -1;
    }
    if (ROW_EOF == rc) {
      break;
    }
    if (field_count_ < 0) {
      // header=false：用第一行数据确定列数，列名 column0, column1, ...
      field_count_ = static_cast<int64_t>(cells.size());
      column_names_.clear();
      for (int64_t i = 0; i < field_count_; ++i) {
        column_names_.push_back(make_column_name(i));
      }
      types.assign(static_cast<size_t>(field_count_), FILE_TYPE_NULL);
      nullable.assign(static_cast<size_t>(field_count_), false);
    }
    if (static_cast<int64_t>(cells.size()) != field_count_) {
      continue;  // 坏行跳过，等读取阶段再报错
    }
    for (int64_t i = 0; i < field_count_; ++i) {
      const FileCell &cell = cells[static_cast<size_t>(i)];
      if (cell.is_null_) {
        nullable[static_cast<size_t>(i)] = true;
      } else {
        const FileLogicalType value_type = file_infer_type(cell.text_);
        types[static_cast<size_t>(i)] =
            file_widen_type(types[static_cast<size_t>(i)], value_type);
      }
    }
  }
  return 0;
}

void CsvFileReader::build_schema(const std::vector<FileLogicalType> &types,
                                 const std::vector<bool> &nullable)
{
  schema_.clear();
  for (size_t i = 0; i < column_names_.size() && i < types.size(); ++i) {
    const bool type_is_null = (FILE_TYPE_NULL == types[i]);
    const bool column_nullable = (i < nullable.size() && nullable[i]) || type_is_null;
    schema_.push_back(FileColumnSchema(column_names_[i], types[i], column_nullable));
  }
}

// 推断阶段已经把文件读了一遍，这里必须把文件位置重置到"数据起点"：
// 回到文件开头 -> 重新跳过 BOM -> 重新跳过表头（若有）。
int CsvFileReader::rewind_to_data_start()
{
  int ret = 0;
  if (0 != ::fseek(fp_, 0, SEEK_SET)) {
    set_error("failed to rewind file '%s': %s", path_.c_str(), ::strerror(errno));
    ret = -1;
  }
  if (0 == ret) {
    buf_pos_ = 0;
    buf_len_ = 0;
    line_no_ = 1;
    row_line_ = 1;
    eof_ = false;
    read_error_ = false;
    ret = skip_bom();
  }
  if (0 == ret && options_.header_ && field_count_ > 0) {
    std::vector<FileCell> cells;
    bool row_eof = false;
    if (ROW_ERR == parse_row(cells, row_eof)) {
      ret = -1;
    }
  }
  return ret;
}

int CsvFileReader::open()
{
  int ret = 0;
  close();
  error_.clear();

  if (path_.empty()) {
    set_error("file path is empty");
    return -1;
  }
  // 目前只支持全量推断：sample_size 必须是 -1
  if (-1 != options_.sample_size_) {
    set_error("sample_size %lld is not supported, only -1 (full file scan) is allowed",
              static_cast<long long>(options_.sample_size_));
    return -1;
  }

  fp_ = ::fopen(path_.c_str(), "rb");
  if (NULL == fp_) {
    set_error("failed to open file '%s': %s", path_.c_str(), ::strerror(errno));
    return -1;
  }
  buffer_.resize(static_cast<size_t>(CSV_READ_BUFFER_SIZE), '\0');
  buf_pos_ = 0;
  buf_len_ = 0;
  opened_ = true;
  eof_ = false;
  read_error_ = false;
  line_no_ = 1;
  row_line_ = 1;
  field_count_ = -1;
  column_names_.clear();
  schema_.clear();

  if (0 == ret) {
    ret = skip_bom();
  }
  if (0 == ret && options_.header_) {
    ret = read_header();
  }

  std::vector<FileLogicalType> types;
  std::vector<bool> nullable;
  if (0 == ret) {
    ret = infer_scan(types, nullable);  // 全量推断（会读完整个文件）
  }
  if (0 == ret) {
    build_schema(types, nullable);
    ret = rewind_to_data_start();  // 回到数据起点，供 next_batch 使用
  }
  if (0 == ret) {
    field_count_ = static_cast<int64_t>(schema_.size());
  }

  if (0 != ret) {
    close();  // 保留 error_ 内容
  }
  return ret;
}

int CsvFileReader::next_batch(int64_t max_rows, std::vector<FileRow> &rows, bool &eof)
{
  int ret = 0;
  rows.clear();
  eof = false;

  if (!opened_ || NULL == fp_) {
    if (error_.empty()) {
      set_error("file reader is not open");
    }
    ret = -1;
  } else if (eof_) {
    eof = true;
  } else if (max_rows <= 0) {
    // 不消费任何数据，也不结束
  } else {
    std::vector<FileCell> cells;
    int64_t produced = 0;
    while (produced < max_rows) {
      bool row_eof = false;
      const int rc = parse_row(cells, row_eof);
      if (ROW_ERR == rc) {
        ret = -1;
        break;
      }
      if (ROW_EOF == rc) {
        eof_ = true;
        eof = true;
        break;
      }
      if (field_count_ >= 0 && static_cast<int64_t>(cells.size()) != field_count_) {
        set_error("line %lld: expected %lld fields but got %lld",
                  static_cast<long long>(row_line_),
                  static_cast<long long>(field_count_),
                  static_cast<long long>(cells.size()));
        ret = -1;
        break;
      }
      rows.push_back(FileRow());
      rows.back().cells_.swap(cells);  // 避免整行拷贝；下一次 parse_row 会重新 clear
      ++produced;
    }
  }
  return ret;
}

void CsvFileReader::close()
{
  if (NULL != fp_) {
    ::fclose(fp_);
    fp_ = NULL;
  }
  opened_ = false;
  eof_ = true;
  read_error_ = false;
  buf_pos_ = 0;
  buf_len_ = 0;
  line_no_ = 1;
  row_line_ = 1;
  field_count_ = -1;
  column_names_.clear();
  schema_.clear();
}

// ============================================================================
// GlobFileReader and unified reader factory
// ============================================================================

class GlobFileReader : public FileFormatReader
{
public:
  explicit GlobFileReader(const std::string &pattern)
    : pattern_(pattern), schema_(), files_(), pos_(0), error_()
  {}
  virtual ~GlobFileReader() {}

  virtual int open() override
  {
    files_.clear();
    schema_.clear();
    pos_ = 0;
    error_.clear();
    schema_.push_back(FileColumnSchema("file", FILE_TYPE_VARCHAR, false));
    return glob_files(pattern_, files_);
  }

  virtual const std::vector<FileColumnSchema> &get_schema() const override
  {
    return schema_;
  }

  virtual int next_batch(int64_t max_rows, std::vector<FileRow> &rows, bool &eof) override
  {
    int ret = 0;
    if (max_rows < 0) {
      ret = -1;
      error_ = "max_rows must not be negative";
    } else {
      while (static_cast<int64_t>(rows.size()) < max_rows && pos_ < files_.size()) {
        FileRow row;
        FileCell cell;
        cell.is_null_ = false;
        cell.text_ = files_.at(pos_++);
        row.cells_.push_back(cell);
        rows.push_back(row);
      }
      eof = pos_ >= files_.size();
    }
    return ret;
  }

  virtual void close() override
  {
    files_.clear();
    schema_.clear();
    pos_ = 0;
  }

  virtual const std::string &error() const override
  {
    return error_;
  }

private:
  std::string pattern_;
  std::vector<FileColumnSchema> schema_;
  std::vector<std::string> files_;
  size_t pos_;
  std::string error_;
};

int create_file_reader(const std::string &path,
                       const FileReadOptions &options,
                       std::unique_ptr<FileFormatReader> &reader)
{
  int ret = 0;
  reader.reset();
  if (FILE_FORMAT_CSV == options.format_) {
    CsvFileReader *csv_reader = new (std::nothrow) CsvFileReader(path, options);
    if (NULL == csv_reader) {
      ret = -1;
    } else {
      reader.reset(csv_reader);
    }
  } else if (FILE_FORMAT_JSONL == options.format_) {
    ret = create_jsonl_file_reader(path, options, reader);
  } else if (FILE_FORMAT_PARQUET == options.format_) {
    ret = create_parquet_file_reader(path, options, reader);
  } else if (FILE_FORMAT_GLOB == options.format_) {
    GlobFileReader *glob_reader = new (std::nothrow) GlobFileReader(path);
    if (NULL == glob_reader) {
      ret = -1;
    } else {
      reader.reset(glob_reader);
    }
  } else {
    ret = -1;
  }
  return ret;
}

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
