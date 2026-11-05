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

// JSONL / NDJSON 读取器（file as database）。
//
// 设计要点：
//   1. 一行一个 JSON 对象，行内只允许扁平字段；value 支持 string / number /
//      true / false / null。出现嵌套对象或数组直接报错。
//   2. schema 是所有行字段名的并集，按字段名在文件中首次出现的顺序排列；
//      缺失字段与显式 null 都输出 NULL。
//   3. 同一字段跨行类型不一致时用 file_widen_type 宽化；cell 的 text_ 始终
//      使用该值的规范文本（string 为解码后的内容，number 为原始 token，
//      bool 为 "true"/"false"）。
//   4. 两遍扫描：open() 先完整扫一遍推断并集 schema 与列类型，然后回绕文件，
//      next_batch() 再顺序读取。内存里只保留当前批次的行。
//   5. 只做"声明"，不实现类型推断函数与 create_file_reader()：它们由
//      ob_file_csv_reader.cpp 提供。

#include "ob_file_format_reader.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <ios>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{
namespace
{

// 本文件内部统一使用的返回码。
const int JSONL_OK = 0;
const int JSONL_ERR = -1;

// 一行的解析结果：既用于第一遍推断（只要 name_ / type_ / is_null_），
// 也用于第二遍取数（还要 text_）。
struct JsonField
{
  JsonField() : name_(), text_(), type_(FILE_TYPE_NULL), is_null_(true) {}
  std::string name_;        // 解码后的 key
  std::string text_;        // 规范文本（string 内容 / number 原文 / "true" / "false"）
  FileLogicalType type_;    // 由 value 推导出的候选逻辑类型
  bool is_null_;            // 显式 null
};

inline bool is_json_space(char c)
{
  return ' ' == c || '\t' == c || '\r' == c || '\n' == c;
}

inline bool is_ascii_digit(char c)
{
  return c >= '0' && c <= '9';
}

inline int hex_value(char c)
{
  if (c >= '0' && c <= '9') {
    return c - '0';
  } else if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// 把一个 Unicode 码点编码成 UTF-8 追加到 out。非法码点（超过 U+10FFFF 或
// 代理区）由调用方提前替换成 U+FFFD，这里只做编码。
void append_utf8(uint32_t code_point, std::string &out)
{
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

// 单行 JSON 对象解析器。只做词法/语法校验与解码，不分配整行副本。
class JsonLineParser
{
public:
  JsonLineParser(const std::string &line, int64_t line_no)
    : line_(line), pos_(0), line_no_(line_no), error_()
  {}
  ~JsonLineParser() {}

  // 解析整行（允许前后空白），成功时 fields 按出现顺序保存每个 key/value，
  // 同名 key 会出现多次（由调用方实现"以最后一个为准"）。
  int parse_object(std::vector<JsonField> &fields);
  const std::string &error() const { return error_; }

private:
  bool at_end() const { return pos_ >= line_.size(); }
  void skip_space()
  {
    while (!at_end() && is_json_space(line_[pos_])) {
      ++pos_;
    }
  }
  int fail(const char *reason);
  int fail(const std::string &reason);
  int fail_invalid(const char *reason);
  int fail_invalid(const std::string &reason);
  int parse_value(JsonField &field);
  int parse_string(std::string &out);
  int parse_hex4(uint32_t &value);
  bool try_parse_low_surrogate(uint32_t &low);
  int parse_number(std::string &out, bool &is_double_token);
  int expect_literal(const char *literal);

  const std::string &line_;
  size_t pos_;
  int64_t line_no_;
  std::string error_;
};

int JsonLineParser::fail(const std::string &reason)
{
  error_.assign("line ");
  error_.append(std::to_string(line_no_));
  error_.append(": ");
  error_.append(reason);
  return JSONL_ERR;
}

int JsonLineParser::fail(const char *reason)
{
  return fail(std::string(reason));
}

int JsonLineParser::fail_invalid(const std::string &reason)
{
  std::string message("invalid JSON: ");
  message.append(reason);
  return fail(message);
}

int JsonLineParser::fail_invalid(const char *reason)
{
  return fail_invalid(std::string(reason));
}

int JsonLineParser::parse_object(std::vector<JsonField> &fields)
{
  fields.clear();
  skip_space();
  if (at_end() || '{' != line_[pos_]) {
    return fail("expected JSON object");
  }
  ++pos_;
  skip_space();
  if (!at_end() && '}' == line_[pos_]) {
    ++pos_;
  } else {
    while (true) {
      skip_space();
      if (at_end()) {
        return fail_invalid("unexpected end of line in object");
      }
      if ('"' != line_[pos_]) {
        return fail_invalid("expected '\"' at start of object key");
      }
      JsonField field;
      int ret = parse_string(field.name_);
      if (JSONL_OK != ret) {
        return ret;
      }
      skip_space();
      if (at_end() || ':' != line_[pos_]) {
        return fail_invalid("expected ':' after object key");
      }
      ++pos_;
      skip_space();
      ret = parse_value(field);
      if (JSONL_OK != ret) {
        return ret;
      }
      fields.push_back(std::move(field));
      skip_space();
      if (at_end()) {
        return fail_invalid("unexpected end of line in object");
      }
      if (',' == line_[pos_]) {
        ++pos_;
        continue;
      }
      if ('}' == line_[pos_]) {
        ++pos_;
        break;
      }
      return fail_invalid("expected ',' or '}' after object value");
    }
  }
  skip_space();
  if (!at_end()) {
    return fail_invalid("unexpected trailing content after JSON object");
  }
  return JSONL_OK;
}

int JsonLineParser::parse_value(JsonField &field)
{
  if (at_end()) {
    return fail_invalid("unexpected end of line after ':'");
  }
  const char c = line_[pos_];
  int ret = JSONL_OK;
  if ('"' == c) {
    ret = parse_string(field.text_);
    if (JSONL_OK != ret) {
      return ret;
    }
    // 字符串也是"内容推断"：与 CSV 共用同一套推断规则，这样 "123" 与
    // 数字 123 得到同一个类型；"2026-08-02" 可以推断成 DATE。
    FileLogicalType type = file_infer_type(field.text_);
    // 空串/纯空白是实实在在的字符串，不应被当成 NULL（file_infer_type
    // 对空输入返回 FILE_TYPE_NULL）。
    if (FILE_TYPE_NULL == type) {
      type = FILE_TYPE_VARCHAR;
    }
    field.type_ = type;
    field.is_null_ = false;
  } else if ('{' == c || '[' == c) {
    return fail("nested object/array is not supported");
  } else if ('t' == c) {
    ret = expect_literal("true");
    if (JSONL_OK != ret) {
      return ret;
    }
    field.text_.assign("true");
    field.type_ = FILE_TYPE_BOOLEAN;
    field.is_null_ = false;
  } else if ('f' == c) {
    ret = expect_literal("false");
    if (JSONL_OK != ret) {
      return ret;
    }
    field.text_.assign("false");
    field.type_ = FILE_TYPE_BOOLEAN;
    field.is_null_ = false;
  } else if ('n' == c) {
    ret = expect_literal("null");
    if (JSONL_OK != ret) {
      return ret;
    }
    field.text_.clear();
    field.type_ = FILE_TYPE_NULL;
    field.is_null_ = true;
  } else if ('-' == c || is_ascii_digit(c)) {
    bool is_double_token = false;
    ret = parse_number(field.text_, is_double_token);
    if (JSONL_OK != ret) {
      return ret;
    }
    FileLogicalType type = file_infer_type(field.text_);
    // 括号保护：number 一定是数值类型，推断函数给出别的结果时退回数值类型。
    if (is_double_token) {
      if (FILE_TYPE_DOUBLE != type) {
        type = FILE_TYPE_DOUBLE;
      }
    } else if (FILE_TYPE_BIGINT != type && FILE_TYPE_DOUBLE != type) {
      type = FILE_TYPE_BIGINT;
    }
    field.type_ = type;
    field.is_null_ = false;
  } else {
    std::string reason("unexpected character '");
    reason.push_back(c);
    reason.append("'");
    return fail_invalid(reason);
  }
  return JSONL_OK;
}

int JsonLineParser::expect_literal(const char *literal)
{
  const size_t length = strlen(literal);
  if (line_.compare(pos_, length, literal) != 0) {
    std::string reason("expected literal \"");
    reason.append(literal).append("\"");
    return fail_invalid(reason);
  }
  pos_ += length;
  return JSONL_OK;
}

int JsonLineParser::parse_hex4(uint32_t &value)
{
  if (pos_ + 4 > line_.size()) {
    return fail_invalid("incomplete \\u escape");
  }
  uint32_t parsed = 0;
  for (int i = 0; i < 4; ++i) {
    const int digit = hex_value(line_[pos_ + i]);
    if (digit < 0) {
      return fail_invalid("invalid hex digit in \\u escape");
    }
    parsed = (parsed << 4) | static_cast<uint32_t>(digit);
  }
  pos_ += 4;
  value = parsed;
  return JSONL_OK;
}

// 只在当前位置确实是一个合法低代理项转义时消费它。
bool JsonLineParser::try_parse_low_surrogate(uint32_t &low)
{
  if (pos_ + 6 > line_.size()) {
    return false;
  }
  if ('\\' != line_[pos_] || 'u' != line_[pos_ + 1]) {
    return false;
  }
  uint32_t parsed = 0;
  for (int i = 0; i < 4; ++i) {
    const int digit = hex_value(line_[pos_ + 2 + i]);
    if (digit < 0) {
      return false;
    }
    parsed = (parsed << 4) | static_cast<uint32_t>(digit);
  }
  if (parsed < 0xDC00 || parsed > 0xDFFF) {
    return false;
  }
  pos_ += 6;
  low = parsed;
  return true;
}

int JsonLineParser::parse_string(std::string &out)
{
  // 调用方保证 line_[pos_] == '"'。
  ++pos_;
  out.clear();
  while (true) {
    if (at_end()) {
      return fail_invalid("unterminated string");
    }
    const unsigned char c = static_cast<unsigned char>(line_[pos_]);
    if ('"' == c) {
      ++pos_;
      return JSONL_OK;
    } else if ('\\' == c) {
      ++pos_;
      if (at_end()) {
        return fail_invalid("unterminated escape sequence");
      }
      const char escape = line_[pos_];
      ++pos_;
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t code_point = 0;
          int ret = parse_hex4(code_point);
          if (JSONL_OK != ret) {
            return ret;
          }
          if (code_point >= 0xD800 && code_point <= 0xDBFF) {
            uint32_t low = 0;
            if (try_parse_low_surrogate(low)) {
              code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
            } else {
              // 孤立的高代理项：替换成 U+FFFD，不吞掉后面的内容。
              code_point = 0xFFFD;
            }
          } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
            // 孤立的低代理项：同样替换成 U+FFFD。
            code_point = 0xFFFD;
          }
          append_utf8(code_point, out);
          break;
        }
        default: {
          std::string reason("invalid escape character '");
          reason.push_back(escape);
          reason.append("'");
          return fail_invalid(reason);
        }
      }
    } else if (c < 0x20) {
      return fail_invalid("unescaped control character in string");
    } else {
      // 非 ASCII 字节按原样透传（假设文件本身是合法 UTF-8）。
      out.push_back(static_cast<char>(c));
      ++pos_;
    }
  }
}

int JsonLineParser::parse_number(std::string &out, bool &is_double_token)
{
  const size_t start = pos_;
  is_double_token = false;
  if (!at_end() && '-' == line_[pos_]) {
    ++pos_;
  }
  if (at_end() || !is_ascii_digit(line_[pos_])) {
    return fail_invalid("invalid number: expected digit");
  }
  if ('0' == line_[pos_]) {
    ++pos_;
    // JSON 不允许前导 0（如 01、-012）。
    if (!at_end() && is_ascii_digit(line_[pos_])) {
      return fail_invalid("invalid number: leading zero");
    }
  } else {
    while (!at_end() && is_ascii_digit(line_[pos_])) {
      ++pos_;
    }
  }
  if (!at_end() && '.' == line_[pos_]) {
    is_double_token = true;
    ++pos_;
    if (at_end() || !is_ascii_digit(line_[pos_])) {
      return fail_invalid("invalid number: expected digit after '.'");
    }
    while (!at_end() && is_ascii_digit(line_[pos_])) {
      ++pos_;
    }
  }
  if (!at_end() && ('e' == line_[pos_] || 'E' == line_[pos_])) {
    is_double_token = true;
    ++pos_;
    if (!at_end() && ('+' == line_[pos_] || '-' == line_[pos_])) {
      ++pos_;
    }
    if (at_end() || !is_ascii_digit(line_[pos_])) {
      return fail_invalid("invalid number: expected digit in exponent");
    }
    while (!at_end() && is_ascii_digit(line_[pos_])) {
      ++pos_;
    }
  }
  out.assign(line_, start, pos_ - start);
  return JSONL_OK;
}

// 判断一行去掉行尾 '\r' 后是否只有空白。
bool is_blank_line(const std::string &line)
{
  for (size_t i = 0; i < line.size(); ++i) {
    if (!is_json_space(line[i])) {
      return false;
    }
  }
  return true;
}

// 去掉行尾 '\r'（兼容 CRLF），并去掉文件首行的 UTF-8 BOM。
void normalize_line(int64_t line_no, std::string &line)
{
  if (!line.empty() && '\r' == line[line.size() - 1]) {
    line.resize(line.size() - 1);
  }
  if (1 == line_no && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
      static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
    line.erase(0, 3);
  }
}

}  // namespace

// JSONL 流式读取器：open() 做完整扫描推断 schema，next_batch() 回绕后取数。
class JsonlFileReader : public FileFormatReader
{
public:
  explicit JsonlFileReader(const std::string &path, const FileReadOptions &options);
  virtual ~JsonlFileReader();

  virtual int open();
  virtual const std::vector<FileColumnSchema> &get_schema() const;
  virtual int next_batch(int64_t max_rows, std::vector<FileRow> &rows, bool &eof);
  virtual void close();
  virtual const std::string &error() const;

private:
  int scan_schema();
  int reset_stream();
  int read_next_line(std::string &line, bool &has_line);
  int find_or_add_column(const std::string &name, int64_t &index);
  bool find_column(const std::string &name, int64_t &index) const;

  std::string path_;
  FileReadOptions options_;
  std::ifstream in_;
  std::vector<FileColumnSchema> schema_;
  std::unordered_map<std::string, int64_t> column_index_;
  std::string error_;
  int64_t line_no_;      // 当前物理行号（含空行）
  int64_t scanned_rows_; // 第一遍已经扫过的数据行数（用于计算 nullable）
  bool opened_;
  bool eof_;
  // 复用缓冲，避免每行重新分配。
  std::vector<JsonField> fields_;
  // 第一遍的每行状态。JSON 里同名 key 以最后一个为准，所以一行内先记录
  // 每个列"最后一次出现"的类型，行末再统一宽化。
  std::vector<char> row_seen_;             // 本行出现过的列
  std::vector<char> pending_seen_;         // 本行该列是否已有候选值
  std::vector<char> pending_null_;         // 本行该列最后一次出现是 null
  std::vector<FileLogicalType> pending_type_;  // 本行该列最后一次出现的类型
  std::vector<char> any_missing_;          // 该列是否曾经缺失
  std::vector<char> any_null_;             // 该列是否出现过显式 null
};

JsonlFileReader::JsonlFileReader(const std::string &path, const FileReadOptions &options)
  : path_(path),
    options_(options),
    in_(),
    schema_(),
    column_index_(),
    error_(),
    line_no_(0),
    scanned_rows_(0),
    opened_(false),
    eof_(true),
    fields_(),
    row_seen_(),
    pending_seen_(),
    pending_null_(),
    pending_type_(),
    any_missing_(),
    any_null_()
{}

JsonlFileReader::~JsonlFileReader()
{
  close();
}

int JsonlFileReader::open()
{
  error_.clear();
  close();
  schema_.clear();
  column_index_.clear();
  row_seen_.clear();
  pending_seen_.clear();
  pending_null_.clear();
  pending_type_.clear();
  any_missing_.clear();
  any_null_.clear();
  fields_.clear();
  line_no_ = 0;
  scanned_rows_ = 0;
  eof_ = false;

  if (-1 != options_.sample_size_) {
    error_.assign("jsonl reader only supports full file scan (sample_size = -1)");
    return JSONL_ERR;
  }

  in_.open(path_.c_str(), std::ios::in | std::ios::binary);
  if (!in_.is_open()) {
    error_.assign("failed to open file '");
    error_.append(path_);
    error_.append("': ");
    error_.append(strerror(errno));
    return JSONL_ERR;
  }

  int ret = scan_schema();
  if (JSONL_OK != ret) {
    in_.close();
    in_.clear();
    return ret;
  }

  ret = reset_stream();
  if (JSONL_OK != ret) {
    in_.close();
    in_.clear();
    return ret;
  }

  // 第一遍已经把 schema 定下来了，这里回绕准备第二遍。
  line_no_ = 0;
  eof_ = false;
  opened_ = true;
  return JSONL_OK;
}

const std::vector<FileColumnSchema> &JsonlFileReader::get_schema() const
{
  return schema_;
}

int JsonlFileReader::scan_schema()
{
  // 第一遍：只关心字段名、字段顺序和每个字段的宽化类型。
  std::string line;
  while (true) {
    bool has_line = false;
    int ret = read_next_line(line, has_line);
    if (JSONL_OK != ret) {
      return ret;
    }
    if (!has_line) {
      break;
    }
    ++line_no_;
    normalize_line(line_no_, line);
    if (is_blank_line(line)) {
      continue;
    }
    fields_.clear();
    JsonLineParser parser(line, line_no_);
    ret = parser.parse_object(fields_);
    if (JSONL_OK != ret) {
      error_ = parser.error();
      return ret;
    }
    std::fill(row_seen_.begin(), row_seen_.end(), 0);
    std::fill(pending_seen_.begin(), pending_seen_.end(), 0);
    for (size_t i = 0; i < fields_.size(); ++i) {
      const JsonField &field = fields_[i];
      int64_t index = -1;
      ret = find_or_add_column(field.name_, index);
      if (JSONL_OK != ret) {
        return ret;
      }
      // find_or_add_column 可能扩容了这些标记数组，这里重新取一遍大小。
      if (static_cast<size_t>(index) >= row_seen_.size()) {
        row_seen_.resize(index + 1, 0);
      }
      row_seen_[index] = 1;
      // 同名 key：后出现的覆盖前面的候选类型，实现"以最后一个为准"。
      pending_seen_[index] = 1;
      pending_null_[index] = field.is_null_ ? 1 : 0;
      pending_type_[index] = field.type_;
    }
    // 本行没有出现的列都按"缺失"记一次，影响 nullable 的推导。
    for (size_t i = 0; i < schema_.size(); ++i) {
      if (!row_seen_[i]) {
        any_missing_[i] = 1;
      } else if (pending_seen_[i]) {
        if (pending_null_[i]) {
          any_null_[i] = 1;
        } else {
          schema_[i].type_ = file_widen_type(schema_[i].type_, pending_type_[i]);
        }
      }
    }
    ++scanned_rows_;
  }

  for (size_t i = 0; i < schema_.size(); ++i) {
    schema_[i].nullable_ = (0 != any_missing_[i] || 0 != any_null_[i]);
  }
  return JSONL_OK;
}

int JsonlFileReader::reset_stream()
{
  in_.clear();
  in_.seekg(0, std::ios::beg);
  if (!in_.good()) {
    error_.assign("failed to rewind file '");
    error_.append(path_);
    error_.append("'");
    return JSONL_ERR;
  }
  return JSONL_OK;
}

int JsonlFileReader::next_batch(int64_t max_rows, std::vector<FileRow> &rows, bool &eof)
{
  rows.clear();
  eof = false;
  if (!opened_) {
    error_.assign("jsonl reader is not open");
    return JSONL_ERR;
  }
  if (eof_) {
    eof = true;
    return JSONL_OK;
  }
  if (max_rows <= 0) {
    return JSONL_OK;
  }

  std::string line;
  int64_t produced = 0;
  while (produced < max_rows) {
    bool has_line = false;
    int ret = read_next_line(line, has_line);
    if (JSONL_OK != ret) {
      return ret;
    }
    if (!has_line) {
      eof_ = true;
      break;
    }
    ++line_no_;
    normalize_line(line_no_, line);
    if (is_blank_line(line)) {
      continue;
    }
    fields_.clear();
    JsonLineParser parser(line, line_no_);
    ret = parser.parse_object(fields_);
    if (JSONL_OK != ret) {
      error_ = parser.error();
      return ret;
    }

    FileRow row;
    row.cells_.resize(schema_.size());
    for (size_t i = 0; i < fields_.size(); ++i) {
      const JsonField &field = fields_[i];
      int64_t index = -1;
      if (!find_column(field.name_, index)) {
        // schema 已固定，理论上不会发生；跳过以保持读取不中断。
        continue;
      }
      FileCell &cell = row.cells_[index];
      if (field.is_null_) {
        // 同名 key 以最后一个为准：后面的 null 要把前面写入的值抹掉。
        cell.is_null_ = true;
        cell.text_.clear();
      } else {
        cell.is_null_ = false;
        cell.text_ = field.text_;
      }
    }
    rows.push_back(std::move(row));
    ++produced;
  }
  eof = eof_;
  return JSONL_OK;
}

void JsonlFileReader::close()
{
  if (in_.is_open()) {
    in_.close();
  }
  in_.clear();
  opened_ = false;
  eof_ = true;
}

const std::string &JsonlFileReader::error() const
{
  return error_;
}

int JsonlFileReader::read_next_line(std::string &line, bool &has_line)
{
  line.clear();
  has_line = false;
  if (!in_.is_open()) {
    return JSONL_OK;
  }
  if (std::getline(in_, line)) {
    has_line = true;
    return JSONL_OK;
  }
  if (in_.bad()) {
    error_.assign("failed to read file '");
    error_.append(path_);
    error_.append("': I/O error");
    return JSONL_ERR;
  }
  return JSONL_OK;
}

int JsonlFileReader::find_or_add_column(const std::string &name, int64_t &index)
{
  std::unordered_map<std::string, int64_t>::const_iterator it = column_index_.find(name);
  if (it != column_index_.end()) {
    index = it->second;
    return JSONL_OK;
  }
  const int64_t new_index = static_cast<int64_t>(schema_.size());
  schema_.push_back(FileColumnSchema(name, FILE_TYPE_NULL, true));
  column_index_.insert(std::make_pair(name, new_index));
  row_seen_.resize(new_index + 1, 0);
  pending_seen_.resize(new_index + 1, 0);
  pending_null_.resize(new_index + 1, 0);
  pending_type_.resize(new_index + 1, FILE_TYPE_NULL);
  any_missing_.resize(new_index + 1, 0);
  any_null_.resize(new_index + 1, 0);
  // 该列第一次出现时如果前面已经有数据行，说明它在前面的行里是缺失的。
  if (scanned_rows_ > 0) {
    any_missing_[new_index] = 1;
  }
  index = new_index;
  return JSONL_OK;
}

bool JsonlFileReader::find_column(const std::string &name, int64_t &index) const
{
  std::unordered_map<std::string, int64_t>::const_iterator it = column_index_.find(name);
  if (it == column_index_.end()) {
    return false;
  }
  index = it->second;
  return true;
}

// 工厂函数：只负责创建 JSONL 读取器，不做 open()。真正的格式分发由
// ob_file_csv_reader.cpp 里的 create_file_reader() 完成。
int create_jsonl_file_reader(const std::string &path,
                             const FileReadOptions &options,
                             std::unique_ptr<FileFormatReader> &reader)
{
  int ret = JSONL_OK;
  reader.reset();
  try {
    std::unique_ptr<JsonlFileReader> jsonl_reader(new (std::nothrow) JsonlFileReader(path, options));
    if (NULL == jsonl_reader.get()) {
      ret = JSONL_ERR;
    } else {
      reader = std::move(jsonl_reader);
    }
  } catch (...) {
    // 构造函数里 std::string 拷贝可能抛 bad_alloc；对外统一返回错误码。
    reader.reset();
    ret = JSONL_ERR;
  }
  return ret;
}

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
