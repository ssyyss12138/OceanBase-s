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

// ob_file_parquet_reader.cpp
//
// "file as database" 用到的 Parquet 读取器，完全自包含、零第三方依赖：
//   * 手写 Thrift Compact Protocol 解析（FileMetaData / RowGroup / PageHeader 等）；
//   * 手写 Snappy 解压（raw block 格式）和 DEFLATE/GZIP 解压（RFC 1951 / RFC 1952）；
//   * 手写 PLAIN、PLAIN_DICTIONARY、RLE_DICTIONARY、RLE(bit-packed hybrid)、
//     BIT_PACKED 解码；
//   * 支持多 row group，按 row group 顺序流式输出。
//
// 只实现 ParquetFileReader / create_parquet_file_reader，
// 不实现 create_file_reader()、类型推断、glob（由其它文件负责）。
//
// 明确不支持（返回 -1 并在 error() 中给出说明）：
//   * REPEATED / 嵌套列（max_rep_level > 0 或 max_def_level > 1）；
//   * DECIMAL / TIME / INT96 / UUID / FLOAT16 / INTERVAL 等逻辑类型；
//   * UNCOMPRESSED / SNAPPY / GZIP 之外的压缩编码（LZO/BROTLI/LZ4/ZSTD）；
//   * DELTA_* / BYTE_STREAM_SPLIT 等编码。
// DATA_PAGE_V2 已尽力支持（代码见 decode_data_page_v2）。

#include "ob_file_format_reader.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{
namespace
{

// ---------------------------------------------------------------------------
// 一、基础工具：little-endian 读取、varint、文本格式化
// ---------------------------------------------------------------------------

inline uint16_t load_le16(const uint8_t *p)
{
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                               (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t load_le32(const uint8_t *p)
{
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t load_le64(const uint8_t *p)
{
  return static_cast<uint64_t>(load_le32(p)) |
         (static_cast<uint64_t>(load_le32(p + 4)) << 32);
}

// 从内存缓冲区读取一个无符号 LEB128 varint（用于 snappy / RLE）。
bool read_uvarint_buf(const uint8_t *p, size_t n, size_t &pos, uint64_t &v)
{
  v = 0;
  int shift = 0;
  while (pos < n && shift <= 63) {
    const uint8_t b = p[pos++];
    v |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (0 == (b & 0x80)) {
      return true;
    }
    shift += 7;
  }
  return false;
}

std::string format_int64(int64_t v)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  return std::string(buf);
}

// 把「1970-01-01 起的天数」格式化成 YYYY-MM-DD（Howard Hinnant 的 civil_from_days 算法）。
std::string format_date_days(int64_t z)
{
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint64_t doe = static_cast<uint64_t>(z - era * 146097);
  const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint64_t mp = (5 * doy + 2) / 153;
  const uint64_t d = doy - (153 * mp + 2) / 5 + 1;
  int64_t m = static_cast<int64_t>(mp) + (mp < 10 ? 3 : -9);
  if (m <= 2) {
    ++y;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld",
           static_cast<long long>(y), static_cast<long long>(m),
           static_cast<long long>(d));
  return std::string(buf);
}

// 把 TIMESTAMP（毫秒/微秒/纳秒）格式化成 "YYYY-MM-DD HH:MM:SS"（丢弃小数秒）。
std::string format_timestamp_value(int64_t value, int32_t unit)
{
  int64_t scale = 1000;             // 0: 毫秒
  if (1 == unit) {
    scale = 1000000;                // 微秒
  } else if (2 == unit) {
    scale = 1000000000;             // 纳秒
  }
  int64_t sec = value / scale;
  int64_t rem = value % scale;
  if (rem < 0) {                    // 保证 floor 语义
    rem += scale;
    --sec;
  }
  int64_t days = sec / 86400;
  int64_t sod = sec % 86400;
  if (sod < 0) {
    sod += 86400;
    --days;
  }
  std::string out = format_date_days(days);
  char buf[32];
  snprintf(buf, sizeof(buf), " %02lld:%02lld:%02lld",
           static_cast<long long>(sod / 3600),
           static_cast<long long>((sod / 60) % 60),
           static_cast<long long>(sod % 60));
  out += buf;
  return out;
}

// 用「最短且可无损回读」的精度格式化 double。
std::string format_double(double v)
{
  if (std::isnan(v)) {
    return "nan";
  }
  if (std::isinf(v)) {
    return v < 0 ? "-inf" : "inf";
  }
  char buf[64];
  for (int prec = 1; prec <= 17; ++prec) {
    snprintf(buf, sizeof(buf), "%.*g", prec, v);
    char *end = nullptr;
    const double back = strtod(buf, &end);
    if (nullptr != end && '\0' == *end && back == v) {
      break;
    }
  }
  return std::string(buf);
}

std::string format_float(float v)
{
  if (std::isnan(v)) {
    return "nan";
  }
  if (std::isinf(v)) {
    return v < 0 ? "-inf" : "inf";
  }
  char buf[64];
  for (int prec = 1; prec <= 9; ++prec) {
    snprintf(buf, sizeof(buf), "%.*g", prec, static_cast<double>(v));
    char *end = nullptr;
    const float back = strtof(buf, &end);
    if (nullptr != end && '\0' == *end && back == v) {
      break;
    }
  }
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// 二、Thrift Compact Protocol 读取器
//
// Compact 协议要点：
//   * 字段头 1 字节：高 4 bit 是字段号增量，低 4 bit 是类型；增量为 0 时
//     后面跟一个 zigzag varint 的绝对字段号；
//   * bool 字段的值直接编码在字段头的类型里（1=true, 2=false），不占额外字节；
//   * bool 作为集合元素时占 1 字节（1 或 2）；
//   * 整数用 zigzag varint，double 用 8 字节 little-endian；
//   * binary 是 varint 长度 + 原始字节；
//   * list/set 头 1 字节：高 4 bit 元素个数（15 表示后面跟 varint 个数），
//     低 4 bit 元素类型；map 头是 varint 个数 + 1 字节 key/value 类型；
//   * 结构体以 STOP(0) 结束，字段号增量是「同一个结构体内」相对上一个字段号的。
// ---------------------------------------------------------------------------

enum CompactType : uint8_t
{
  CT_STOP = 0,
  CT_TRUE = 1,
  CT_FALSE = 2,
  CT_BYTE = 3,
  CT_I16 = 4,
  CT_I32 = 5,
  CT_I64 = 6,
  CT_DOUBLE = 7,
  CT_BINARY = 8,
  CT_LIST = 9,
  CT_SET = 10,
  CT_MAP = 11,
  CT_STRUCT = 12
};

class CompactReader
{
public:
  CompactReader(const uint8_t *data, size_t size)
      : data_(data), size_(size), pos_(0), eof_(false)
  {}

  bool read_byte(uint8_t &v)
  {
    if (pos_ >= size_) {
      eof_ = true;
      return false;
    }
    v = data_[pos_++];
    return true;
  }

  bool read_uvarint(uint64_t &v)
  {
    v = 0;
    int shift = 0;
    for (;;) {
      uint8_t b = 0;
      if (!read_byte(b)) {
        return false;
      }
      if (shift > 63) {
        return false;
      }
      v |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (0 == (b & 0x80)) {
        return true;
      }
      shift += 7;
    }
  }

  bool read_zigzag(int64_t &v)
  {
    uint64_t u = 0;
    if (!read_uvarint(u)) {
      return false;
    }
    v = static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
    return true;
  }

  bool read_binary(std::string &s)
  {
    uint64_t len = 0;
    if (!read_uvarint(len)) {
      return false;
    }
    if (len > static_cast<uint64_t>(size_ - pos_)) {
      pos_ = size_;
      eof_ = true;
      return false;
    }
    s.assign(reinterpret_cast<const char *>(data_ + pos_), static_cast<size_t>(len));
    pos_ += static_cast<size_t>(len);
    return true;
  }

  bool skip(size_t n)
  {
    if (n > size_ - pos_) {
      pos_ = size_;
      eof_ = true;
      return false;
    }
    pos_ += n;
    return true;
  }

  size_t pos() const { return pos_; }
  bool eof() const { return eof_; }

private:
  const uint8_t *data_;
  size_t size_;
  size_t pos_;
  bool eof_;
};

// 读字段头。返回 1=成功，0=STOP，-1=错误。
int read_field_header(CompactReader &r, int16_t &last_fid, uint8_t &type, int16_t &fid)
{
  uint8_t b = 0;
  if (!r.read_byte(b)) {
    return -1;
  }
  type = static_cast<uint8_t>(b & 0x0f);
  if (CT_STOP == type) {
    return 0;
  }
  const uint8_t delta = static_cast<uint8_t>(b >> 4);
  if (0 == delta) {
    int64_t v = 0;
    if (!r.read_zigzag(v) || v < INT16_MIN || v > INT16_MAX) {
      return -1;
    }
    fid = static_cast<int16_t>(v);
  } else {
    fid = static_cast<int16_t>(last_fid + delta);
  }
  last_fid = fid;
  if (type > CT_STRUCT) {
    return -1;
  }
  return 1;
}

bool skip_struct(CompactReader &r);

// 跳过集合里的一个值（bool 占 1 字节）。
bool skip_value(CompactReader &r, uint8_t type)
{
  bool ok = false;
  switch (type) {
    case CT_TRUE:
    case CT_FALSE:
    case CT_BYTE: {
      uint8_t b = 0;
      ok = r.read_byte(b);
      break;
    }
    case CT_I16:
    case CT_I32:
    case CT_I64: {
      int64_t v = 0;
      ok = r.read_zigzag(v);
      break;
    }
    case CT_DOUBLE:
      ok = r.skip(8);
      break;
    case CT_BINARY: {
      uint64_t len = 0;
      ok = r.read_uvarint(len) && r.skip(static_cast<size_t>(len));
      break;
    }
    case CT_LIST:
    case CT_SET: {
      uint8_t h = 0;
      if (!r.read_byte(h)) {
        return false;
      }
      uint64_t count = h >> 4;
      const uint8_t et = static_cast<uint8_t>(h & 0x0f);
      if (15 == count && !r.read_uvarint(count)) {
        return false;
      }
      ok = true;
      for (uint64_t i = 0; i < count && ok; ++i) {
        ok = skip_value(r, et);
      }
      break;
    }
    case CT_MAP: {
      uint64_t count = 0;
      if (!r.read_uvarint(count)) {
        return false;
      }
      ok = true;
      if (count > 0) {
        uint8_t kv = 0;
        if (!r.read_byte(kv)) {
          return false;
        }
        for (uint64_t i = 0; i < count && ok; ++i) {
          ok = skip_value(r, static_cast<uint8_t>(kv >> 4));
          if (ok) {
            ok = skip_value(r, static_cast<uint8_t>(kv & 0x0f));
          }
        }
      }
      break;
    }
    case CT_STRUCT:
      ok = skip_struct(r);
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

// 跳过结构体字段的值（bool 的值在字段头里，不占字节）。
bool skip_field_value(CompactReader &r, uint8_t type)
{
  if (CT_TRUE == type || CT_FALSE == type) {
    return true;
  }
  return skip_value(r, type);
}

bool skip_struct(CompactReader &r)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    if (!skip_field_value(r, type)) {
      return false;
    }
  }
}

// 字段值读取小工具：类型不匹配时按实际类型跳过，保证前向兼容。
bool read_i32_field(CompactReader &r, uint8_t type, int32_t &field)
{
  if (CT_I32 != type && CT_I16 != type && CT_I64 != type) {
    return skip_field_value(r, type);
  }
  int64_t v = 0;
  if (!r.read_zigzag(v)) {
    return false;
  }
  field = static_cast<int32_t>(v);
  return true;
}

bool read_i64_field(CompactReader &r, uint8_t type, int64_t &field)
{
  if (CT_I64 != type && CT_I32 != type && CT_I16 != type) {
    return skip_field_value(r, type);
  }
  int64_t v = 0;
  if (!r.read_zigzag(v)) {
    return false;
  }
  field = v;
  return true;
}

bool read_bool_field(CompactReader &r, uint8_t type, bool &field)
{
  if (CT_TRUE == type) {
    field = true;
    return true;
  }
  if (CT_FALSE == type) {
    field = false;
    return true;
  }
  return skip_field_value(r, type);
}

bool read_binary_field(CompactReader &r, uint8_t type, std::string &field)
{
  if (CT_BINARY != type) {
    return skip_field_value(r, type);
  }
  return r.read_binary(field);
}

// list<i32>
bool read_i32_list(CompactReader &r, std::vector<int32_t> &out)
{
  uint8_t h = 0;
  if (!r.read_byte(h)) {
    return false;
  }
  uint64_t count = h >> 4;
  const uint8_t et = static_cast<uint8_t>(h & 0x0f);
  if (15 == count && !r.read_uvarint(count)) {
    return false;
  }
  out.clear();
  out.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    if (CT_I32 == et || CT_I16 == et || CT_I64 == et) {
      int64_t v = 0;
      if (!r.read_zigzag(v)) {
        return false;
      }
      out.push_back(static_cast<int32_t>(v));
    } else if (!skip_value(r, et)) {
      return false;
    }
  }
  return true;
}

// list<string>
bool read_string_list(CompactReader &r, std::vector<std::string> &out)
{
  uint8_t h = 0;
  if (!r.read_byte(h)) {
    return false;
  }
  uint64_t count = h >> 4;
  const uint8_t et = static_cast<uint8_t>(h & 0x0f);
  if (15 == count && !r.read_uvarint(count)) {
    return false;
  }
  out.clear();
  out.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    if (CT_BINARY == et) {
      std::string s;
      if (!r.read_binary(s)) {
        return false;
      }
      out.push_back(s);
    } else if (!skip_value(r, et)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// 三、Parquet 元数据结构
// ---------------------------------------------------------------------------

// 逻辑类型（LogicalType 是一个 union，这里只保留我们关心的信息）。
struct LogicalTypeInfo
{
  enum Kind
  {
    LT_NONE = 0,
    LT_STRING,
    LT_MAP,
    LT_LIST,
    LT_ENUM,
    LT_DECIMAL,
    LT_DATE,
    LT_TIME,
    LT_TIMESTAMP,
    LT_INTEGER,
    LT_UNKNOWN,
    LT_JSON,
    LT_BSON,
    LT_UUID,
    LT_FLOAT16
  };
  Kind kind;
  int32_t time_unit;   // 仅 TIME/TIMESTAMP：0=毫秒 1=微秒 2=纳秒
  int32_t bit_width;   // 仅 INTEGER
  bool is_signed;      // 仅 INTEGER
  int32_t scale;       // 仅 DECIMAL
  int32_t precision;   // 仅 DECIMAL
  LogicalTypeInfo()
      : kind(LT_NONE), time_unit(0), bit_width(0), is_signed(true), scale(0), precision(0)
  {}
};

// parquet 的 SchemaElement（对应 thrift 结构体）。
struct SchemaElement
{
  int32_t type;             // 物理类型，-1 表示未设置（组节点）
  int32_t type_length;      // FIXED_LEN_BYTE_ARRAY 的字节长度
  int32_t repetition;       // 0=REQUIRED 1=OPTIONAL 2=REPEATED
  std::string name;
  int32_t num_children;     // 组节点的孩子数
  int32_t converted_type;   // 旧式 converted_type，-1 表示未设置
  LogicalTypeInfo logical;
  SchemaElement()
      : type(-1), type_length(0), repetition(-1), num_children(-1), converted_type(-1)
  {}
};

// ColumnMetaData：一个 row group 内某一列的数据页位置 / 编码 / 压缩方式。
struct ColumnMeta
{
  int32_t type;                 // 物理类型
  std::vector<int32_t> encodings;
  std::vector<std::string> path; // path_in_schema
  int32_t codec;                // 压缩编码
  int64_t num_values;           // 该 chunk 的值个数（含 null）
  int64_t total_uncompressed_size;
  int64_t total_compressed_size;
  int64_t data_page_offset;
  int64_t dict_page_offset;
  bool has_dict_page_offset;
  ColumnMeta()
      : type(-1), codec(0), num_values(0), total_uncompressed_size(0),
        total_compressed_size(0), data_page_offset(0), dict_page_offset(-1),
        has_dict_page_offset(false)
  {}
};

struct RowGroupMeta
{
  std::vector<ColumnMeta> columns;
  int64_t total_byte_size;
  int64_t num_rows;
  RowGroupMeta() : total_byte_size(0), num_rows(0) {}
};

struct FileMeta
{
  int32_t version;
  std::vector<SchemaElement> schema;
  int64_t num_rows;
  std::vector<RowGroupMeta> row_groups;
  FileMeta() : version(0), num_rows(0) {}
};

// PageHeader 中我们会用到的字段（statistics/crc 之类全部跳过）。
struct PageHeaderInfo
{
  int32_t type;
  int32_t uncompressed_size;
  int32_t compressed_size;
  int32_t num_values;       // 数据页的值个数（含 null）
  int32_t encoding;         // 数据页 values 的编码
  int32_t def_encoding;     // v1 定义级别编码
  int32_t rep_encoding;     // v1 重复级别编码
  int32_t dict_num_values;  // 字典页条目数
  int32_t dict_encoding;    // 字典页编码
  bool is_v2;
  int32_t v2_num_nulls;
  int32_t v2_num_rows;
  int32_t v2_def_len;
  int32_t v2_rep_len;
  bool v2_is_compressed;
  PageHeaderInfo()
      : type(-1), uncompressed_size(-1), compressed_size(-1), num_values(-1),
        encoding(-1), def_encoding(-1), rep_encoding(-1), dict_num_values(-1),
        dict_encoding(-1), is_v2(false), v2_num_nulls(0), v2_num_rows(0),
        v2_def_len(0), v2_rep_len(0), v2_is_compressed(true)
  {}
};

// ---- 各 thrift 结构体的解析函数 ----

bool parse_logical_type(CompactReader &r, LogicalTypeInfo &out);

// TimeUnit / TimestampType / TimeType：field 1 是 isAdjustedToUTC，field 2 是 unit。
bool parse_time_type(CompactReader &r, LogicalTypeInfo &out)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    if (2 == fid && CT_STRUCT == type) {
      // TimeUnit 是 union：1=MILLIS, 2=MICROS, 3=NANOS，每个分支都是空结构体。
      int16_t last2 = 0;
      for (;;) {
        uint8_t type2 = 0;
        int16_t fid2 = 0;
        const int rc2 = read_field_header(r, last2, type2, fid2);
        if (rc2 < 0) {
          return false;
        }
        if (0 == rc2) {
          break;
        }
        if (fid2 >= 1 && fid2 <= 3 && CT_STRUCT == type2) {
          out.time_unit = fid2 - 1;
          if (!skip_struct(r)) {
            return false;
          }
        } else if (!skip_field_value(r, type2)) {
          return false;
        }
      }
    } else if (!skip_field_value(r, type)) {
      return false;
    }
  }
}

// DecimalType：1=scale, 2=precision。
bool parse_decimal_type(CompactReader &r, LogicalTypeInfo &out)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    if (1 == fid && !read_i32_field(r, type, out.scale)) {
      return false;
    } else if (2 == fid && !read_i32_field(r, type, out.precision)) {
      return false;
    } else if (1 != fid && 2 != fid && !skip_field_value(r, type)) {
      return false;
    }
  }
}

// IntType：1=bitWidth(i8), 2=isSigned(bool)。
bool parse_int_type(CompactReader &r, LogicalTypeInfo &out)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    if (1 == fid) {
      int32_t bw = 0;
      if (!read_i32_field(r, type, bw)) {
        return false;
      }
      out.bit_width = bw;
    } else if (2 == fid) {
      if (!read_bool_field(r, type, out.is_signed)) {
        return false;
      }
    } else if (!skip_field_value(r, type)) {
      return false;
    }
  }
}

bool parse_logical_type(CompactReader &r, LogicalTypeInfo &out)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    if (CT_STRUCT != type) {
      if (!skip_field_value(r, type)) {
        return false;
      }
      continue;
    }
    switch (fid) {
      case 1: out.kind = LogicalTypeInfo::LT_STRING;  if (!skip_struct(r)) return false; break;
      case 2: out.kind = LogicalTypeInfo::LT_MAP;     if (!skip_struct(r)) return false; break;
      case 3: out.kind = LogicalTypeInfo::LT_LIST;    if (!skip_struct(r)) return false; break;
      case 4: out.kind = LogicalTypeInfo::LT_ENUM;    if (!skip_struct(r)) return false; break;
      case 5:
        out.kind = LogicalTypeInfo::LT_DECIMAL;
        if (!parse_decimal_type(r, out)) return false;
        break;
      case 6: out.kind = LogicalTypeInfo::LT_DATE;      if (!skip_struct(r)) return false; break;
      case 7:
        out.kind = LogicalTypeInfo::LT_TIME;
        if (!parse_time_type(r, out)) return false;
        break;
      case 8:
        out.kind = LogicalTypeInfo::LT_TIMESTAMP;
        if (!parse_time_type(r, out)) return false;
        break;
      case 9:
        out.kind = LogicalTypeInfo::LT_INTEGER;
        if (!parse_int_type(r, out)) return false;
        break;
      case 10: out.kind = LogicalTypeInfo::LT_UNKNOWN;  if (!skip_struct(r)) return false; break;
      case 11: out.kind = LogicalTypeInfo::LT_JSON;     if (!skip_struct(r)) return false; break;
      case 12: out.kind = LogicalTypeInfo::LT_BSON;     if (!skip_struct(r)) return false; break;
      case 13: out.kind = LogicalTypeInfo::LT_UUID;     if (!skip_struct(r)) return false; break;
      case 14: out.kind = LogicalTypeInfo::LT_FLOAT16;  if (!skip_struct(r)) return false; break;
      default: if (!skip_struct(r)) return false; break;
    }
  }
}

bool parse_schema_element(CompactReader &r, SchemaElement &e)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    switch (fid) {
      case 1:
        if (!read_i32_field(r, type, e.type)) return false;
        break;
      case 2:
        if (!read_i32_field(r, type, e.type_length)) return false;
        break;
      case 3:
        if (!read_i32_field(r, type, e.repetition)) return false;
        break;
      case 4:
        if (!read_binary_field(r, type, e.name)) return false;
        break;
      case 5:
        if (!read_i32_field(r, type, e.num_children)) return false;
        break;
      case 6:
        if (!read_i32_field(r, type, e.converted_type)) return false;
        break;
      case 10:
        if (CT_STRUCT == type) {
          if (!parse_logical_type(r, e.logical)) return false;
        } else if (!skip_field_value(r, type)) {
          return false;
        }
        break;
      default:
        if (!skip_field_value(r, type)) return false;
        break;
    }
  }
}

bool parse_column_meta(CompactReader &r, ColumnMeta &cm)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    switch (fid) {
      case 1:
        if (!read_i32_field(r, type, cm.type)) return false;
        break;
      case 2:
        if (CT_LIST != type) {
          if (!skip_field_value(r, type)) return false;
        } else if (!read_i32_list(r, cm.encodings)) {
          return false;
        }
        break;
      case 3:
        if (CT_LIST != type) {
          if (!skip_field_value(r, type)) return false;
        } else if (!read_string_list(r, cm.path)) {
          return false;
        }
        break;
      case 4:
        if (!read_i32_field(r, type, cm.codec)) return false;
        break;
      case 5:
        if (!read_i64_field(r, type, cm.num_values)) return false;
        break;
      case 6:
        if (!read_i64_field(r, type, cm.total_uncompressed_size)) return false;
        break;
      case 7:
        if (!read_i64_field(r, type, cm.total_compressed_size)) return false;
        break;
      case 9:
        if (!read_i64_field(r, type, cm.data_page_offset)) return false;
        break;
      case 11:
        if (!read_i64_field(r, type, cm.dict_page_offset)) return false;
        cm.has_dict_page_offset = true;
        break;
      default:
        if (!skip_field_value(r, type)) return false;
        break;
    }
  }
}

bool parse_column_chunk(CompactReader &r, ColumnMeta &cm)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    if (3 == fid && CT_STRUCT == type) {
      if (!parse_column_meta(r, cm)) {
        return false;
      }
    } else if (!skip_field_value(r, type)) {
      return false;
    }
  }
}

bool parse_row_group(CompactReader &r, RowGroupMeta &rg)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    if (1 == fid && CT_LIST == type) {
      uint8_t h = 0;
      if (!r.read_byte(h)) {
        return false;
      }
      uint64_t count = h >> 4;
      const uint8_t et = static_cast<uint8_t>(h & 0x0f);
      if (15 == count && !r.read_uvarint(count)) {
        return false;
      }
      rg.columns.clear();
      rg.columns.reserve(static_cast<size_t>(count));
      for (uint64_t i = 0; i < count; ++i) {
        if (CT_STRUCT != et) {
          if (!skip_value(r, et)) {
            return false;
          }
        } else {
          ColumnMeta cm;
          if (!parse_column_chunk(r, cm)) {
            return false;
          }
          rg.columns.push_back(cm);
        }
      }
    } else if (2 == fid) {
      if (!read_i64_field(r, type, rg.total_byte_size)) return false;
    } else if (3 == fid) {
      if (!read_i64_field(r, type, rg.num_rows)) return false;
    } else if (!skip_field_value(r, type)) {
      return false;
    }
  }
}

bool parse_file_metadata(CompactReader &r, FileMeta &out)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    switch (fid) {
      case 1:
        if (!read_i32_field(r, type, out.version)) return false;
        break;
      case 2: {
        if (CT_LIST != type) {
          if (!skip_field_value(r, type)) return false;
          break;
        }
        uint8_t h = 0;
        if (!r.read_byte(h)) return false;
        uint64_t count = h >> 4;
        const uint8_t et = static_cast<uint8_t>(h & 0x0f);
        if (15 == count && !r.read_uvarint(count)) return false;
        out.schema.clear();
        out.schema.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
          if (CT_STRUCT != et) {
            if (!skip_value(r, et)) return false;
          } else {
            SchemaElement e;
            if (!parse_schema_element(r, e)) return false;
            out.schema.push_back(e);
          }
        }
        break;
      }
      case 3:
        if (!read_i64_field(r, type, out.num_rows)) return false;
        break;
      case 4: {
        if (CT_LIST != type) {
          if (!skip_field_value(r, type)) return false;
          break;
        }
        uint8_t h = 0;
        if (!r.read_byte(h)) return false;
        uint64_t count = h >> 4;
        const uint8_t et = static_cast<uint8_t>(h & 0x0f);
        if (15 == count && !r.read_uvarint(count)) return false;
        out.row_groups.clear();
        out.row_groups.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
          if (CT_STRUCT != et) {
            if (!skip_value(r, et)) return false;
          } else {
            RowGroupMeta rg;
            if (!parse_row_group(r, rg)) return false;
            out.row_groups.push_back(rg);
          }
        }
        break;
      }
      default:
        if (!skip_field_value(r, type)) return false;
        break;
    }
  }
}

bool parse_data_page_header(CompactReader &r, PageHeaderInfo &ph)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    switch (fid) {
      case 1: if (!read_i32_field(r, type, ph.num_values)) return false; break;
      case 2: if (!read_i32_field(r, type, ph.encoding)) return false; break;
      case 3: if (!read_i32_field(r, type, ph.def_encoding)) return false; break;
      case 4: if (!read_i32_field(r, type, ph.rep_encoding)) return false; break;
      default: if (!skip_field_value(r, type)) return false; break;
    }
  }
}

bool parse_dict_page_header(CompactReader &r, PageHeaderInfo &ph)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    switch (fid) {
      case 1: if (!read_i32_field(r, type, ph.dict_num_values)) return false; break;
      case 2: if (!read_i32_field(r, type, ph.dict_encoding)) return false; break;
      default: if (!skip_field_value(r, type)) return false; break;
    }
  }
}

bool parse_data_page_v2_header(CompactReader &r, PageHeaderInfo &ph)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      return true;
    }
    switch (fid) {
      case 1:
        if (!read_i32_field(r, type, ph.num_values)) return false;
        break;
      case 2:
        if (!read_i32_field(r, type, ph.v2_num_nulls)) return false;
        break;
      case 3:
        if (!read_i32_field(r, type, ph.v2_num_rows)) return false;
        break;
      case 4:
        if (!read_i32_field(r, type, ph.encoding)) return false;
        break;
      case 5:
        if (!read_i32_field(r, type, ph.v2_def_len)) return false;
        break;
      case 6:
        if (!read_i32_field(r, type, ph.v2_rep_len)) return false;
        break;
      case 7:
        if (!read_bool_field(r, type, ph.v2_is_compressed)) return false;
        break;
      default:
        if (!skip_field_value(r, type)) return false;
        break;
    }
    ph.is_v2 = true;
  }
}

bool parse_page_header(CompactReader &r, PageHeaderInfo &ph)
{
  int16_t last = 0;
  for (;;) {
    uint8_t type = 0;
    int16_t fid = 0;
    const int rc = read_field_header(r, last, type, fid);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      break;
    }
    switch (fid) {
      case 1: if (!read_i32_field(r, type, ph.type)) return false; break;
      case 2: if (!read_i32_field(r, type, ph.uncompressed_size)) return false; break;
      case 3: if (!read_i32_field(r, type, ph.compressed_size)) return false; break;
      case 5:
        if (CT_STRUCT != type) {
          if (!skip_field_value(r, type)) return false;
        } else if (!parse_data_page_header(r, ph)) {
          return false;
        }
        break;
      case 7:
        if (CT_STRUCT != type) {
          if (!skip_field_value(r, type)) return false;
        } else if (!parse_dict_page_header(r, ph)) {
          return false;
        }
        break;
      case 8:
        if (CT_STRUCT != type) {
          if (!skip_field_value(r, type)) return false;
        } else if (!parse_data_page_v2_header(r, ph)) {
          return false;
        }
        break;
      default:
        if (!skip_field_value(r, type)) return false;
        break;
    }
  }
  return ph.type >= 0 && ph.compressed_size >= 0 && ph.uncompressed_size >= 0;
}

// ---------------------------------------------------------------------------
// 四、解压：Snappy（raw block）与 DEFLATE/GZIP
// ---------------------------------------------------------------------------

// Snappy 原始块解压。格式：varint 解压后长度 + 一串 tag（literal / copy）。
bool snappy_decompress(const uint8_t *src, size_t n, std::vector<uint8_t> &out)
{
  out.clear();
  size_t pos = 0;
  uint64_t ulen = 0;
  if (!read_uvarint_buf(src, n, pos, ulen)) {
    return false;
  }
  // 防御：单页解压后不会超过 2GB
  if (ulen > (static_cast<uint64_t>(1) << 31)) {
    return false;
  }
  out.reserve(static_cast<size_t>(ulen));
  while (pos < n) {
    const uint8_t tag = src[pos++];
    uint32_t len = 0;
    uint32_t offset = 0;
    switch (tag & 0x03) {
      case 0: {  // literal：短长度 1..60；否则用 1..4 字节长度
        len = static_cast<uint32_t>(tag >> 2);
        if (len < 60) {
          len += 1;
        } else {
          const int extra = static_cast<int>(len) - 59;
          if (pos + static_cast<size_t>(extra) > n) {
            return false;
          }
          uint32_t v = 0;
          for (int i = 0; i < extra; ++i) {
            v |= static_cast<uint32_t>(src[pos + static_cast<size_t>(i)]) << (8 * i);
          }
          pos += static_cast<size_t>(extra);
          len = v + 1;
        }
        if (static_cast<uint64_t>(pos) + len > n) {
          return false;
        }
        out.insert(out.end(), src + pos, src + pos + len);
        pos += len;
        break;
      }
      case 1: {  // copy：offset 11 bit，长度 4..11
        if (pos + 1 > n) {
          return false;
        }
        len = ((tag >> 2) & 0x07) + 4;
        offset = (static_cast<uint32_t>(tag >> 5) << 8) | src[pos];
        pos += 1;
        break;
      }
      case 2: {  // copy：offset 16 bit，长度 1..64
        if (pos + 2 > n) {
          return false;
        }
        len = static_cast<uint32_t>(tag >> 2) + 1;
        offset = load_le16(src + pos);
        pos += 2;
        break;
      }
      default: {  // copy：offset 32 bit，长度 1..64
        if (pos + 4 > n) {
          return false;
        }
        len = static_cast<uint32_t>(tag >> 2) + 1;
        offset = load_le32(src + pos);
        pos += 4;
        break;
      }
    }
    if ((tag & 0x03) != 0) {
      if (0 == offset || offset > out.size()) {
        return false;
      }
      const size_t from = out.size() - offset;
      for (uint32_t i = 0; i < len; ++i) {
        out.push_back(out[from + i]);  // 允许重叠（逐字节复制）
      }
    }
    if (out.size() > ulen) {
      return false;
    }
  }
  return out.size() == ulen;
}

// DEFLATE 位读取器（LSB first）。
class BitReader
{
public:
  BitReader(const uint8_t *data, size_t size)
      : data_(data), size_(size), pos_(0), buf_(0), cnt_(0)
  {}

  bool get_bits(int nbits, uint32_t &value)
  {
    if (nbits <= 0) {
      value = 0;
      return true;
    }
    while (cnt_ < nbits) {
      if (pos_ >= size_) {
        return false;
      }
      buf_ |= static_cast<uint32_t>(data_[pos_++]) << cnt_;
      cnt_ += 8;
    }
    value = buf_ & ((1u << nbits) - 1u);
    buf_ >>= nbits;
    cnt_ -= nbits;
    return true;
  }

  bool read_byte_raw(uint8_t &b)
  {
    if (pos_ >= size_) {
      return false;
    }
    b = data_[pos_++];
    return true;
  }

  // 丢弃当前不足一字节的位，并把已读入缓冲的整字节「退回」输入流。
  void align_to_byte()
  {
    const int drop = cnt_ % 8;
    buf_ >>= drop;
    cnt_ -= drop;
    pos_ -= static_cast<size_t>(cnt_ / 8);
    buf_ = 0;
    cnt_ = 0;
  }

private:
  const uint8_t *data_;
  size_t size_;
  size_t pos_;
  uint32_t buf_;
  int cnt_;
};

// 规范 Huffman 表：counts/first_code/offsets 描述每个码长的码字分布。
struct HuffmanTable
{
  int counts[16];
  int first_code[16];
  int offsets[16];
  std::vector<int> symbols;
  HuffmanTable()
  {
    memset(counts, 0, sizeof(counts));
    memset(first_code, 0, sizeof(first_code));
    memset(offsets, 0, sizeof(offsets));
  }
};

bool build_huffman(const uint8_t *lengths, int n, HuffmanTable &h)
{
  h = HuffmanTable();
  for (int i = 0; i < n; ++i) {
    if (lengths[i] > 15) {
      return false;
    }
    ++h.counts[lengths[i]];
  }
  int total = 0;
  for (int len = 1; len <= 15; ++len) {
    total += h.counts[len];
  }
  if (0 == total) {
    return false;
  }
  h.counts[0] = 0;
  // 检测码空间溢出（over-subscribed）。
  int left = 1;
  for (int len = 1; len <= 15; ++len) {
    left <<= 1;
    left -= h.counts[len];
    if (left < 0) {
      return false;
    }
  }
  h.first_code[1] = 0;
  h.offsets[1] = 0;
  for (int len = 2; len <= 15; ++len) {
    h.first_code[len] = (h.first_code[len - 1] + h.counts[len - 1]) << 1;
    h.offsets[len] = h.offsets[len - 1] + h.counts[len - 1];
  }
  h.symbols.assign(static_cast<size_t>(total), 0);
  int next[16];
  memset(next, 0, sizeof(next));
  for (int len = 1; len <= 15; ++len) {
    next[len] = h.offsets[len];
  }
  for (int i = 0; i < n; ++i) {
    const int len = lengths[i];
    if (len > 0) {
      h.symbols[static_cast<size_t>(next[len]++)] = i;
    }
  }
  return true;
}

int decode_symbol(BitReader &br, const HuffmanTable &h)
{
  int code = 0;
  for (int len = 1; len <= 15; ++len) {
    uint32_t bit = 0;
    if (!br.get_bits(1, bit)) {
      return -1;
    }
    code = (code << 1) | static_cast<int>(bit);
    const int cnt = h.counts[len];
    if (cnt > 0) {
      const int idx = code - h.first_code[len];
      if (idx >= 0 && idx < cnt) {
        return h.symbols[static_cast<size_t>(h.offsets[len] + idx)];
      }
    }
  }
  return -1;
}

const int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                           6145, 8193, 12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_codes(BitReader &br, const HuffmanTable &lit, const HuffmanTable &dist,
                   std::vector<uint8_t> &out)
{
  for (;;) {
    const int sym = decode_symbol(br, lit);
    if (sym < 0) {
      return false;
    }
    if (sym < 256) {
      out.push_back(static_cast<uint8_t>(sym));
    } else if (256 == sym) {
      return true;
    } else if (sym < 257 || sym > 285) {
      return false;
    } else {
      const int li = sym - 257;
      uint32_t extra = 0;
      if (kLenExtra[li] > 0 && !br.get_bits(kLenExtra[li], extra)) {
        return false;
      }
      const int len = kLenBase[li] + static_cast<int>(extra);
      const int dsym = decode_symbol(br, dist);
      if (dsym < 0 || dsym > 29) {
        return false;
      }
      uint32_t dextra = 0;
      if (kDistExtra[dsym] > 0 && !br.get_bits(kDistExtra[dsym], dextra)) {
        return false;
      }
      const size_t d = static_cast<size_t>(kDistBase[dsym]) + dextra;
      if (d > out.size()) {
        return false;
      }
      const size_t from = out.size() - d;
      for (int k = 0; k < len; ++k) {
        out.push_back(out[from + static_cast<size_t>(k)]);
      }
    }
  }
}

bool inflate_stored(BitReader &br, std::vector<uint8_t> &out)
{
  br.align_to_byte();
  uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
  if (!br.read_byte_raw(b0) || !br.read_byte_raw(b1) ||
      !br.read_byte_raw(b2) || !br.read_byte_raw(b3)) {
    return false;
  }
  const int len = static_cast<int>(b0) | (static_cast<int>(b1) << 8);
  const int nlen = static_cast<int>(b2) | (static_cast<int>(b3) << 8);
  if (((len ^ 0xffff) & 0xffff) != nlen) {
    return false;
  }
  for (int i = 0; i < len; ++i) {
    uint8_t b = 0;
    if (!br.read_byte_raw(b)) {
      return false;
    }
    out.push_back(b);
  }
  return true;
}

bool build_fixed_tables(HuffmanTable &lit, HuffmanTable &dist)
{
  uint8_t lens[288];
  for (int i = 0; i < 144; ++i) lens[i] = 8;
  for (int i = 144; i < 256; ++i) lens[i] = 9;
  for (int i = 256; i < 280; ++i) lens[i] = 7;
  for (int i = 280; i < 288; ++i) lens[i] = 8;
  if (!build_huffman(lens, 288, lit)) {
    return false;
  }
  uint8_t dlens[32];
  for (int i = 0; i < 32; ++i) dlens[i] = 5;
  return build_huffman(dlens, 32, dist);
}

bool inflate_dynamic(BitReader &br, std::vector<uint8_t> &out)
{
  uint32_t hlit = 0, hdist = 0, hclen = 0;
  if (!br.get_bits(5, hlit) || !br.get_bits(5, hdist) || !br.get_bits(4, hclen)) {
    return false;
  }
  const int nlit = static_cast<int>(hlit) + 257;
  const int ndist = static_cast<int>(hdist) + 1;
  const int nclen = static_cast<int>(hclen) + 4;
  static const int kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4,
                                 12, 3, 13, 2, 14, 1, 15};
  uint8_t clen[19];
  memset(clen, 0, sizeof(clen));
  for (int i = 0; i < nclen; ++i) {
    uint32_t v = 0;
    if (!br.get_bits(3, v)) {
      return false;
    }
    clen[kOrder[i]] = static_cast<uint8_t>(v);
  }
  HuffmanTable clh;
  if (!build_huffman(clen, 19, clh)) {
    return false;
  }
  std::vector<uint8_t> lens(static_cast<size_t>(nlit + ndist), 0);
  int i = 0;
  while (i < nlit + ndist) {
    const int sym = decode_symbol(br, clh);
    if (sym < 0) {
      return false;
    }
    if (sym < 16) {
      lens[static_cast<size_t>(i++)] = static_cast<uint8_t>(sym);
    } else if (16 == sym) {
      if (0 == i) {
        return false;
      }
      uint32_t rep = 0;
      if (!br.get_bits(2, rep)) {
        return false;
      }
      int cnt = 3 + static_cast<int>(rep);
      if (i + cnt > nlit + ndist) {
        return false;
      }
      const uint8_t prev = lens[static_cast<size_t>(i - 1)];
      while (cnt-- > 0) {
        lens[static_cast<size_t>(i++)] = prev;
      }
    } else if (17 == sym) {
      uint32_t rep = 0;
      if (!br.get_bits(3, rep)) {
        return false;
      }
      int cnt = 3 + static_cast<int>(rep);
      if (i + cnt > nlit + ndist) {
        return false;
      }
      while (cnt-- > 0) {
        lens[static_cast<size_t>(i++)] = 0;
      }
    } else if (18 == sym) {
      uint32_t rep = 0;
      if (!br.get_bits(7, rep)) {
        return false;
      }
      int cnt = 11 + static_cast<int>(rep);
      if (i + cnt > nlit + ndist) {
        return false;
      }
      while (cnt-- > 0) {
        lens[static_cast<size_t>(i++)] = 0;
      }
    } else {
      return false;
    }
  }
  HuffmanTable lit;
  if (!build_huffman(lens.data(), nlit, lit)) {
    return false;
  }
  // 距离树可能为空（只有字面量），此时构造一个空表，用到时再报错。
  HuffmanTable dist;
  bool dist_empty = true;
  for (int k = nlit; k < nlit + ndist; ++k) {
    if (lens[static_cast<size_t>(k)] > 0) {
      dist_empty = false;
      break;
    }
  }
  if (!dist_empty && !build_huffman(lens.data() + nlit, ndist, dist)) {
    return false;
  }
  if (dist_empty) {
    memset(dist.counts, 0, sizeof(dist.counts));
    dist.symbols.clear();
  }
  return inflate_codes(br, lit, dist, out);
}

bool inflate_raw(const uint8_t *src, size_t n, std::vector<uint8_t> &out)
{
  BitReader br(src, n);
  bool last = false;
  while (!last) {
    uint32_t bfinal = 0, btype = 0;
    if (!br.get_bits(1, bfinal) || !br.get_bits(2, btype)) {
      return false;
    }
    last = (bfinal != 0);
    if (0 == btype) {
      if (!inflate_stored(br, out)) {
        return false;
      }
    } else if (1 == btype) {
      HuffmanTable lit, dist;
      if (!build_fixed_tables(lit, dist) || !inflate_codes(br, lit, dist, out)) {
        return false;
      }
    } else if (2 == btype) {
      if (!inflate_dynamic(br, out)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

// GZIP（RFC 1952）：10 字节头 + DEFLATE + CRC32/ISIZE。
bool gzip_decompress(const uint8_t *src, size_t n, std::vector<uint8_t> &out)
{
  out.clear();
  if (n < 18 || 0x1f != src[0] || 0x8b != src[1] || 8 != src[2]) {
    return false;
  }
  const uint8_t flg = src[3];
  size_t pos = 10;
  if (flg & 0x04) {  // FEXTRA
    if (pos + 2 > n) {
      return false;
    }
    const size_t xlen = load_le16(src + pos);
    pos += 2 + xlen;
  }
  if (flg & 0x08) {  // FNAME
    while (pos < n && 0 != src[pos]) {
      ++pos;
    }
    ++pos;
  }
  if (flg & 0x10) {  // FCOMMENT
    while (pos < n && 0 != src[pos]) {
      ++pos;
    }
    ++pos;
  }
  if (flg & 0x02) {  // FHCRC
    pos += 2;
  }
  if (pos > n || n - pos < 8) {
    return false;
  }
  if (!inflate_raw(src + pos, n - 8 - pos, out)) {
    return false;
  }
  const uint32_t isize = load_le32(src + n - 4);
  if (static_cast<uint32_t>(out.size() & 0xffffffffULL) != isize) {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 五、编码解码：RLE/bit-packed hybrid、deprecated BIT_PACKED、PLAIN
// ---------------------------------------------------------------------------

// RLE/bit-packed hybrid（用于 definition levels 和字典索引）。
// bit_width=0 表示所有值都是 0，此时 RLE 段不占字节、bit-packed 段也不占字节。
bool decode_rle_hybrid(const uint8_t *p, size_t n, int bit_width, size_t count,
                       std::vector<uint32_t> &out)
{
  out.assign(count, 0);
  if (bit_width < 0 || bit_width > 32) {
    return false;
  }
  const uint32_t mask = (32 == bit_width) ? 0xffffffffu
                                          : ((static_cast<uint32_t>(1) << bit_width) - 1u);
  size_t pos = 0;
  size_t produced = 0;
  while (produced < count) {
    uint64_t header = 0;
    if (!read_uvarint_buf(p, n, pos, header)) {
      return false;
    }
    if (header & 1) {
      // bit-packed run：每组的字节数恰好等于 bit_width（8 个值）。
      const uint64_t groups = header >> 1;
      if (0 == groups) {
        return false;
      }
      const size_t nbytes = static_cast<size_t>(groups) * static_cast<size_t>(bit_width);
      if (nbytes > n - pos) {
        return false;
      }
      const size_t off = pos;
      for (size_t g = 0; g < static_cast<size_t>(groups); ++g) {
        const size_t group_off = off + g * static_cast<size_t>(bit_width);
        for (int k = 0; k < 8 && produced < count; ++k) {
          const size_t bitpos = static_cast<size_t>(k) * static_cast<size_t>(bit_width);
          const size_t byte = group_off + bitpos / 8;
          const int shift = static_cast<int>(bitpos % 8);
          uint64_t chunk = 0;
          for (int b = 0; b < 5 && byte + static_cast<size_t>(b) < n; ++b) {
            chunk |= static_cast<uint64_t>(p[byte + static_cast<size_t>(b)]) << (8 * b);
          }
          out[produced++] = static_cast<uint32_t>((chunk >> shift) & mask);
        }
      }
      pos += nbytes;
    } else {
      // RLE run：值用 ceil(bit_width/8) 字节 little-endian 存储。
      const uint64_t run = header >> 1;
      if (0 == run) {
        return false;
      }
      const int nbytes = (bit_width + 7) / 8;
      if (static_cast<size_t>(nbytes) > n - pos) {
        return false;
      }
      uint32_t v = 0;
      for (int b = 0; b < nbytes; ++b) {
        v |= static_cast<uint32_t>(p[pos + static_cast<size_t>(b)]) << (8 * b);
      }
      v &= mask;
      pos += static_cast<size_t>(nbytes);
      for (uint64_t i = 0; i < run && produced < count; ++i) {
        out[produced++] = v;
      }
    }
  }
  return true;
}

// 老式（已废弃）BIT_PACKED 级别编码：值从每个字节的 MSB 开始连续紧密排列。
bool decode_bit_packed_levels(const uint8_t *p, size_t n, int bit_width, size_t count,
                              std::vector<uint32_t> &out)
{
  out.assign(count, 0);
  if (0 == bit_width) {
    return true;
  }
  const size_t need_bits = count * static_cast<size_t>(bit_width);
  if ((need_bits + 7) / 8 > n) {
    return false;
  }
  size_t bit = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t v = 0;
    for (int b = 0; b < bit_width; ++b) {
      const size_t idx = bit + static_cast<size_t>(b);
      v = (v << 1) | ((p[idx / 8] >> (7 - idx % 8)) & 1u);
    }
    out[i] = v;
    bit += static_cast<size_t>(bit_width);
  }
  return true;
}

// 一列 schema 叶子信息（本读取器只接受扁平 schema）。
struct LeafColumn
{
  std::string name;              // path_in_schema 的叶子名
  std::vector<std::string> path;  // 完整路径
  int32_t physical_type;
  int32_t type_length;
  int32_t converted_type;
  LogicalTypeInfo logical;
  int32_t max_def_level;
  int32_t max_rep_level;
  int32_t time_unit;             // 仅 TIMESTAMP
  FileLogicalType out_type;
  bool nullable;
  LeafColumn()
      : physical_type(-1), type_length(0), converted_type(-1), max_def_level(0),
        max_rep_level(0), time_unit(0), out_type(FILE_TYPE_NULL), nullable(false)
  {}
};

// PLAIN 解码：直接把 count 个值转成规范文本（DATE/TIMESTAMP 在这里格式化）。
bool decode_plain_text(const LeafColumn &leaf, const uint8_t *p, size_t n, size_t &pos,
                       int64_t count, std::vector<std::string> &out)
{
  if (count < 0 || pos > n) {
    return false;
  }
  out.clear();
  out.reserve(static_cast<size_t>(count));
  switch (leaf.physical_type) {
    case 0: {  // BOOLEAN：每个值 1 bit，LSB 优先
      const size_t bytes = (static_cast<size_t>(count) + 7) / 8;
      if (bytes > n - pos) {
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        const bool v = 0 != ((p[pos + static_cast<size_t>(i) / 8] >> (i % 8)) & 1);
        out.push_back(v ? "true" : "false");
      }
      pos += bytes;
      break;
    }
    case 1: {  // INT32
      if (static_cast<uint64_t>(count) * 4 > n - pos) {
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        const int32_t v = static_cast<int32_t>(load_le32(p + pos));
        pos += 4;
        if (FILE_TYPE_DATE == leaf.out_type) {
          out.push_back(format_date_days(v));
        } else {
          out.push_back(format_int64(v));
        }
      }
      break;
    }
    case 2: {  // INT64
      if (static_cast<uint64_t>(count) * 8 > n - pos) {
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        const int64_t v = static_cast<int64_t>(load_le64(p + pos));
        pos += 8;
        if (FILE_TYPE_DATETIME == leaf.out_type) {
          out.push_back(format_timestamp_value(v, leaf.time_unit));
        } else {
          out.push_back(format_int64(v));
        }
      }
      break;
    }
    case 4: {  // FLOAT
      if (static_cast<uint64_t>(count) * 4 > n - pos) {
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        float f = 0;
        memcpy(&f, p + pos, sizeof(f));
        pos += 4;
        out.push_back(format_float(f));
      }
      break;
    }
    case 5: {  // DOUBLE
      if (static_cast<uint64_t>(count) * 8 > n - pos) {
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        double d = 0;
        memcpy(&d, p + pos, sizeof(d));
        pos += 8;
        out.push_back(format_double(d));
      }
      break;
    }
    case 6: {  // BYTE_ARRAY：4 字节长度 + 字节内容
      for (int64_t i = 0; i < count; ++i) {
        if (n - pos < 4) {
          return false;
        }
        const uint32_t len = load_le32(p + pos);
        pos += 4;
        if (static_cast<uint64_t>(len) > n - pos) {
          return false;
        }
        out.push_back(std::string(reinterpret_cast<const char *>(p + pos), len));
        pos += len;
      }
      break;
    }
    case 7: {  // FIXED_LEN_BYTE_ARRAY
      if (leaf.type_length <= 0) {
        return false;
      }
      const size_t len = static_cast<size_t>(leaf.type_length);
      for (int64_t i = 0; i < count; ++i) {
        if (len > n - pos) {
          return false;
        }
        out.push_back(std::string(reinterpret_cast<const char *>(p + pos), len));
        pos += len;
      }
      break;
    }
    default:
      return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 六、ParquetFileReader：文件布局、schema、row group、page
// ---------------------------------------------------------------------------

class ParquetFileReader : public FileFormatReader
{
public:
  explicit ParquetFileReader(const std::string &path)
      : path_(path), file_size_(0), rg_row_index_(0), next_rg_(0), opened_(false)
  {}

  virtual ~ParquetFileReader() { close(); }

  int open() override;
  const std::vector<FileColumnSchema> &get_schema() const override { return schema_; }
  int next_batch(int64_t max_rows, std::vector<FileRow> &rows, bool &eof) override;
  void close() override;
  const std::string &error() const override { return error_; }

private:
  int fail(const char *fmt, ...)
  {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    error_ = buf;
    return -1;
  }

  // 文件 IO
  bool read_at(int64_t offset, void *dst, size_t len);

  // schema
  int build_columns();
  int build_schema_node(size_t &idx, std::vector<std::string> &path, int depth,
                        int def_level, int rep_level);
  int resolve_leaf_type(LeafColumn &leaf);

  // 页
  int parse_page_header_at(int64_t offset, PageHeaderInfo &ph, int64_t &header_size);
  int decompress_page(int32_t codec, const uint8_t *src, size_t src_len,
                      size_t expect_size, const char *what, std::vector<uint8_t> &out);
  int load_row_group(size_t rg_index);
  int decode_column(const ColumnMeta &cm, const LeafColumn &leaf, int64_t row_count,
                    std::vector<FileCell> &cells);
  int decode_data_page_v1(const LeafColumn &leaf, const PageHeaderInfo &ph,
                          const std::vector<uint8_t> &page, int64_t row_count,
                          std::vector<FileCell> &cells);
  int decode_data_page_v2(const LeafColumn &leaf, const ColumnMeta &cm,
                          const PageHeaderInfo &ph, const std::vector<uint8_t> &raw,
                          int64_t row_count, std::vector<FileCell> &cells);
  int decode_values(const LeafColumn &leaf, int32_t encoding, const uint8_t *p,
                    size_t n, size_t &pos, size_t count,
                    std::vector<std::string> &texts);

  std::string path_;
  std::ifstream in_;
  int64_t file_size_;
  FileMeta meta_;
  std::vector<FileColumnSchema> schema_;
  std::vector<LeafColumn> leaves_;
  std::map<std::string, size_t> path_to_leaf_;
  std::vector<FileRow> rg_rows_;   // 当前 row group 的行缓存（一次只驻留一个 row group）
  size_t rg_row_index_;
  size_t next_rg_;
  bool opened_;
  std::string error_;
  std::vector<std::string> dict_texts_;  // 当前列 chunk 的字典（文本形式）
};

bool ParquetFileReader::read_at(int64_t offset, void *dst, size_t len)
{
  if (offset < 0 || len > static_cast<size_t>(file_size_ - offset)) {
    return false;
  }
  in_.clear();
  in_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in_.good()) {
    return false;
  }
  if (len > 0) {
    in_.read(static_cast<char *>(dst), static_cast<std::streamsize>(len));
    if (in_.gcount() != static_cast<std::streamsize>(len)) {
      return false;
    }
  }
  return true;
}

int ParquetFileReader::open()
{
  int ret = 0;
  close();
  error_.clear();
  in_.open(path_.c_str(), std::ios::in | std::ios::binary);
  if (!in_.is_open()) {
    return fail("无法打开 parquet 文件 '%s'", path_.c_str());
  }
  in_.seekg(0, std::ios::end);
  const std::streamoff size = in_.tellg();
  if (size < 0) {
    return fail("无法获取 parquet 文件 '%s' 的大小", path_.c_str());
  }
  file_size_ = static_cast<int64_t>(size);
  // 最小长度：PAR1 + footer + len(4) + PAR1
  if (file_size_ < 12) {
    return fail("文件 '%s' 太小，不是合法的 parquet 文件", path_.c_str());
  }
  uint8_t magic[4];
  if (!read_at(0, magic, sizeof(magic)) || 0 != memcmp(magic, "PAR1", 4)) {
    return fail("文件 '%s' 的头部魔数不是 PAR1", path_.c_str());
  }
  uint8_t tail[8];
  if (!read_at(file_size_ - 8, tail, sizeof(tail))) {
    return fail("读取文件 '%s' 的尾部失败", path_.c_str());
  }
  if (0 != memcmp(tail + 4, "PAR1", 4)) {
    return fail("文件 '%s' 的尾部魔数不是 PAR1", path_.c_str());
  }
  const uint32_t footer_len = load_le32(tail);
  if (0 == footer_len || static_cast<int64_t>(footer_len) > file_size_ - 12) {
    return fail("文件 '%s' 的 footer 长度 %u 非法", path_.c_str(), footer_len);
  }
  std::vector<uint8_t> footer(static_cast<size_t>(footer_len));
  const int64_t footer_offset = file_size_ - 8 - static_cast<int64_t>(footer_len);
  if (!read_at(footer_offset, footer.data(), footer.size())) {
    return fail("读取文件 '%s' 的 footer 失败", path_.c_str());
  }
  CompactReader cr(footer.data(), footer.size());
  if (!parse_file_metadata(cr, meta_)) {
    return fail("解析 parquet FileMetaData 失败（%s）",
                cr.eof() ? "footer 数据不完整" : "thrift 结构非法");
  }
  if (meta_.schema.empty()) {
    return fail("parquet 文件的 schema 为空");
  }
  if (meta_.num_rows < 0) {
    return fail("parquet 文件的 num_rows 非法: %lld",
                static_cast<long long>(meta_.num_rows));
  }
  if (0 != (ret = build_columns())) {
    // build_columns 已经设置 error_
  } else {
    opened_ = true;
  }
  return ret;
}

int ParquetFileReader::build_columns()
{
  int ret = 0;
  schema_.clear();
  leaves_.clear();
  path_to_leaf_.clear();
  size_t idx = 0;
  std::vector<std::string> path;
  ret = build_schema_node(idx, path, 0, 0, 0);
  if (0 == ret && idx != meta_.schema.size()) {
    ret = fail("parquet schema 元素个数与 num_children 不一致（%zu/%zu）",
               idx, meta_.schema.size());
  }
  if (0 == ret && leaves_.empty()) {
    ret = fail("parquet schema 中没有任何叶子列");
  }
  return ret;
}

// 递归展开 schema。path 不含根节点名；depth=0 表示根节点。
int ParquetFileReader::build_schema_node(size_t &idx, std::vector<std::string> &path,
                                         int depth, int def_level, int rep_level)
{
  int ret = 0;
  if (idx >= meta_.schema.size()) {
    return fail("parquet schema 树不完整（缺少 SchemaElement）");
  }
  const SchemaElement &e = meta_.schema[idx++];
  const bool is_root = (0 == depth);
  if (!is_root) {
    path.push_back(e.name);
    if (1 == e.repetition) {
      ++def_level;
    } else if (2 == e.repetition) {
      ++rep_level;
    } else if (0 != e.repetition && -1 != e.repetition) {
      return fail("列 '%s' 的 repetition_type %d 非法",
                  path.back().c_str(), e.repetition);
    }
  }
  const int num_children = e.num_children < 0 ? 0 : e.num_children;
  if (num_children > 0) {
    for (int i = 0; 0 == ret && i < num_children; ++i) {
      ret = build_schema_node(idx, path, depth + 1, def_level, rep_level);
    }
  } else {
    if (e.type < 0) {
      return fail("列 '%s' 缺少物理类型（type）",
                  path.empty() ? "" : path.back().c_str());
    }
    // 本读取器只支持扁平 schema：不允许 REPEATED，也不允许二层以上的 OPTIONAL。
    if (rep_level > 0) {
      return fail("列 '%s' 是 REPEATED/嵌套列，暂不支持", path.back().c_str());
    }
    if (def_level > 1) {
      return fail("列 '%s' 是嵌套 OPTIONAL 列，暂不支持", path.back().c_str());
    }
    LeafColumn leaf;
    leaf.name = path.back();
    leaf.path = path;
    leaf.physical_type = e.type;
    leaf.type_length = e.type_length;
    leaf.converted_type = e.converted_type;
    leaf.logical = e.logical;
    leaf.max_def_level = def_level;
    leaf.max_rep_level = rep_level;
    leaf.nullable = (def_level > 0);
    if (0 != (ret = resolve_leaf_type(leaf))) {
      // resolve_leaf_type 已经设置 error_
    } else {
      const size_t leaf_index = leaves_.size();
      leaves_.push_back(leaf);
      FileColumnSchema column(leaf.name, leaf.out_type, leaf.nullable);
      schema_.push_back(column);
      // path 用 '.' 连接作为 key（ColumnMeta 的 path_in_schema 是同一语义）
      std::string key;
      for (size_t i = 0; i < leaf.path.size(); ++i) {
        if (i > 0) {
          key += '.';
        }
        key += leaf.path[i];
      }
      if (path_to_leaf_.end() == path_to_leaf_.find(key)) {
        path_to_leaf_[key] = leaf_index;
      }
    }
  }
  if (!is_root) {
    path.pop_back();
  }
  return ret;
}

// 把 parquet 物理类型 + logical/converted 类型映射成 FileLogicalType。
int ParquetFileReader::resolve_leaf_type(LeafColumn &leaf)
{
  const int32_t ct = leaf.converted_type;
  const LogicalTypeInfo &lt = leaf.logical;
  if (LogicalTypeInfo::LT_DECIMAL == lt.kind || 5 == ct) {
    return fail("列 '%s' 使用 DECIMAL 类型，暂不支持", leaf.name.c_str());
  }
  if (LogicalTypeInfo::LT_TIME == lt.kind || 7 == ct || 8 == ct) {
    return fail("列 '%s' 使用 TIME 类型，暂不支持", leaf.name.c_str());
  }
  if (LogicalTypeInfo::LT_UUID == lt.kind || LogicalTypeInfo::LT_FLOAT16 == lt.kind) {
    return fail("列 '%s' 使用 %s 类型，暂不支持", leaf.name.c_str(),
                LogicalTypeInfo::LT_UUID == lt.kind ? "UUID" : "FLOAT16");
  }
  if (21 == ct) {
    return fail("列 '%s' 使用 INTERVAL 类型，暂不支持", leaf.name.c_str());
  }
  if (LogicalTypeInfo::LT_INTEGER == lt.kind && 64 == lt.bit_width && !lt.is_signed) {
    return fail("列 '%s' 是 64 位无符号整数，超出 BIGINT 范围，暂不支持", leaf.name.c_str());
  }
  switch (leaf.physical_type) {
    case 0:  // BOOLEAN
      leaf.out_type = FILE_TYPE_BOOLEAN;
      break;
    case 1: {  // INT32
      const bool is_date = (6 == ct || LogicalTypeInfo::LT_DATE == lt.kind);
      const bool is_ts = (9 == ct || 10 == ct || LogicalTypeInfo::LT_TIMESTAMP == lt.kind);
      if (is_date) {
        leaf.out_type = FILE_TYPE_DATE;
      } else if (is_ts) {
        leaf.out_type = FILE_TYPE_DATETIME;
      } else {
        leaf.out_type = FILE_TYPE_BIGINT;
      }
      break;
    }
    case 2: {  // INT64
      const bool is_ts = (9 == ct || 10 == ct || LogicalTypeInfo::LT_TIMESTAMP == lt.kind);
      leaf.out_type = is_ts ? FILE_TYPE_DATETIME : FILE_TYPE_BIGINT;
      break;
    }
    case 4:  // FLOAT
    case 5:  // DOUBLE
      leaf.out_type = FILE_TYPE_DOUBLE;
      break;
    case 6:  // BYTE_ARRAY
    case 7:  // FIXED_LEN_BYTE_ARRAY
      leaf.out_type = FILE_TYPE_VARCHAR;
      break;
    case 3:  // INT96
      return fail("列 '%s' 使用 INT96 类型，暂不支持", leaf.name.c_str());
    default:
      return fail("列 '%s' 的物理类型 %d 未知", leaf.name.c_str(), leaf.physical_type);
  }
  // TIMESTAMP 的时间单位：logical 优先，其次看 converted_type。
  if (FILE_TYPE_DATETIME == leaf.out_type) {
    if (LogicalTypeInfo::LT_TIMESTAMP == lt.kind) {
      leaf.time_unit = lt.time_unit;
    } else {
      leaf.time_unit = (10 == ct) ? 1 : 0;
    }
  }
  return 0;
}

int ParquetFileReader::parse_page_header_at(int64_t offset, PageHeaderInfo &ph,
                                            int64_t &header_size)
{
  int ret = 0;
  // 页头长度不定（statistics 可能很大），先给 512 字节，解析到一半不够就翻倍重试。
  int64_t try_size = 512;
  for (;;) {
    const int64_t avail = file_size_ - offset;
    if (avail <= 0) {
      ret = fail("页头偏移 %lld 越界", static_cast<long long>(offset));
      break;
    }
    const int64_t want = try_size < avail ? try_size : avail;
    std::vector<uint8_t> buf(static_cast<size_t>(want));
    if (!read_at(offset, buf.data(), buf.size())) {
      ret = fail("读取页头失败（偏移 %lld）", static_cast<long long>(offset));
      break;
    }
    CompactReader cr(buf.data(), buf.size());
    ph = PageHeaderInfo();
    if (parse_page_header(cr, ph)) {
      header_size = static_cast<int64_t>(cr.pos());
      break;
    }
    if (cr.eof() && want == try_size && try_size < (static_cast<int64_t>(1) << 22)) {
      try_size *= 4;
      continue;
    }
    ret = fail("解析 PageHeader 失败（偏移 %lld）", static_cast<long long>(offset));
    break;
  }
  return ret;
}

int ParquetFileReader::decompress_page(int32_t codec, const uint8_t *src, size_t src_len,
                                       size_t expect_size, const char *what,
                                       std::vector<uint8_t> &out)
{
  int ret = 0;
  out.clear();
  switch (codec) {
    case 0:  // UNCOMPRESSED
      out.assign(src, src + src_len);
      break;
    case 1:  // SNAPPY
      if (!snappy_decompress(src, src_len, out)) {
        ret = fail("%s SNAPPY 解压失败", what);
      }
      break;
    case 2:  // GZIP
      if (!gzip_decompress(src, src_len, out)) {
        ret = fail("%s GZIP 解压失败", what);
      }
      break;
    case 3:
      ret = fail("不支持 LZO 压缩（%s）", what);
      break;
    case 4:
      ret = fail("不支持 BROTLI 压缩（%s）", what);
      break;
    case 5:
      ret = fail("不支持 LZ4 压缩（%s）", what);
      break;
    case 6:
      ret = fail("不支持 ZSTD 压缩（%s）", what);
      break;
    case 7:
      ret = fail("不支持 LZ4_RAW 压缩（%s）", what);
      break;
    default:
      ret = fail("未知压缩编码 %d（%s）", codec, what);
      break;
  }
  if (0 == ret && out.size() != expect_size) {
    ret = fail("%s 解压后大小 %zu 与页头声明 %zu 不一致", what, out.size(), expect_size);
  }
  return ret;
}

// 解码一列在某个 row group 内的所有值，产出 row_count 个 FileCell。
int ParquetFileReader::decode_column(const ColumnMeta &cm, const LeafColumn &leaf,
                                     int64_t row_count, std::vector<FileCell> &cells)
{
  int ret = 0;
  cells.clear();
  dict_texts_.clear();
  if (cm.type != leaf.physical_type) {
    return fail("列 '%s' 的物理类型（chunk=%d schema=%d）不一致",
                leaf.name.c_str(), cm.type, leaf.physical_type);
  }
  if (cm.total_compressed_size <= 0) {
    return fail("列 '%s' 的 total_compressed_size 非法: %lld", leaf.name.c_str(),
                static_cast<long long>(cm.total_compressed_size));
  }
  int64_t start = cm.data_page_offset;
  if (cm.has_dict_page_offset && cm.dict_page_offset > 0) {
    start = cm.dict_page_offset;
  }
  if (start <= 0 || start >= file_size_) {
    return fail("列 '%s' 的数据页偏移 %lld 非法", leaf.name.c_str(),
                static_cast<long long>(start));
  }
  int64_t end = start + cm.total_compressed_size;
  if (end > file_size_) {
    end = file_size_;
  }
  int64_t pos = start;
  cells.reserve(static_cast<size_t>(row_count));
  while (static_cast<int64_t>(cells.size()) < row_count && pos < end) {
    PageHeaderInfo ph;
    int64_t header_size = 0;
    if (0 != (ret = parse_page_header_at(pos, ph, header_size))) {
      break;
    }
    pos += header_size;
    if (ph.compressed_size < 0 || ph.uncompressed_size < 0) {
      ret = fail("列 '%s' 的页大小非法", leaf.name.c_str());
      break;
    }
    if (static_cast<int64_t>(ph.compressed_size) > file_size_ - pos) {
      ret = fail("列 '%s' 的页数据越界（偏移 %lld，长度 %d）", leaf.name.c_str(),
                 static_cast<long long>(pos), ph.compressed_size);
      break;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(ph.compressed_size));
    if (!raw.empty() && !read_at(pos, raw.data(), raw.size())) {
      ret = fail("列 '%s' 读取页数据失败", leaf.name.c_str());
      break;
    }
    pos += ph.compressed_size;
    if (2 == ph.type) {
      // DICTIONARY_PAGE：整页用 PLAIN 编码，解压后解码成字典文本。
      if (ph.dict_num_values < 0) {
        ret = fail("列 '%s' 的字典页条目数非法", leaf.name.c_str());
        break;
      }
      std::vector<uint8_t> plain;
      if (0 != (ret = decompress_page(cm.codec, raw.data(), raw.size(),
                                      static_cast<size_t>(ph.uncompressed_size),
                                      "字典页", plain))) {
        break;
      }
      if (0 != ph.dict_encoding && 2 != ph.dict_encoding) {
        ret = fail("列 '%s' 的字典页使用了不支持的编码 %d", leaf.name.c_str(),
                   ph.dict_encoding);
        break;
      }
      size_t dpos = 0;
      std::vector<std::string> dict;
      if (!decode_plain_text(leaf, plain.data(), plain.size(), dpos,
                             ph.dict_num_values, dict)) {
        ret = fail("列 '%s' 的字典页解码失败", leaf.name.c_str());
        break;
      }
      dict_texts_.swap(dict);
    } else if (0 == ph.type) {
      // DATA_PAGE (v1)
      std::vector<uint8_t> page;
      if (0 != (ret = decompress_page(cm.codec, raw.data(), raw.size(),
                                      static_cast<size_t>(ph.uncompressed_size),
                                      "数据页", page))) {
        break;
      }
      if (0 != (ret = decode_data_page_v1(leaf, ph, page, row_count, cells))) {
        break;
      }
    } else if (3 == ph.type) {
      // DATA_PAGE_V2：levels 不压缩，只有 values 可能压缩，直接在 decode 内处理。
      if (0 != (ret = decode_data_page_v2(leaf, cm, ph, raw, row_count, cells))) {
        break;
      }
    } else if (1 == ph.type) {
      // INDEX_PAGE：本读取器不做谓词下推，直接跳过。
    } else {
      ret = fail("列 '%s' 遇到未知页类型 %d", leaf.name.c_str(), ph.type);
      break;
    }
  }
  if (0 == ret && static_cast<int64_t>(cells.size()) != row_count) {
    ret = fail("列 '%s' 的值数量 %zu 与 row group 行数 %lld 不一致",
               leaf.name.c_str(), cells.size(), static_cast<long long>(row_count));
  }
  return ret;
}

int ParquetFileReader::decode_values(const LeafColumn &leaf, int32_t encoding,
                                     const uint8_t *p, size_t n, size_t &pos,
                                     size_t count, std::vector<std::string> &texts)
{
  int ret = 0;
  texts.clear();
  if (0 == count) {
    return 0;
  }
  if (0 == encoding) {
    // PLAIN
    if (!decode_plain_text(leaf, p, n, pos, static_cast<int64_t>(count), texts)) {
      ret = fail("列 '%s' 的 PLAIN 值解码失败", leaf.name.c_str());
    }
  } else if (3 == encoding) {
    // RLE：BOOLEAN 值也可以直接用 RLE/bit-packed hybrid 编码（bit width=1），
    // 与 definition levels 一样带 4 字节长度前缀。
    if (0 != leaf.physical_type) {
      ret = fail("列 '%s' 使用了 RLE 编码，但只有 BOOLEAN 列支持", leaf.name.c_str());
    } else if (n - pos < 4) {
      ret = fail("列 '%s' 的 RLE 值长度前缀缺失", leaf.name.c_str());
    } else {
      const uint32_t len = load_le32(p + pos);
      pos += 4;
      if (static_cast<uint64_t>(len) > n - pos) {
        ret = fail("列 '%s' 的 RLE 值数据越界", leaf.name.c_str());
      } else {
        std::vector<uint32_t> bits;
        if (!decode_rle_hybrid(p + pos, len, 1, count, bits)) {
          ret = fail("列 '%s' 的 RLE 布尔值解码失败", leaf.name.c_str());
        } else {
          texts.reserve(count);
          for (size_t i = 0; i < count; ++i) {
            texts.push_back(0 != bits[i] ? "true" : "false");
          }
          pos += len;
        }
      }
    }
  } else if (2 == encoding || 8 == encoding) {
    // PLAIN_DICTIONARY / RLE_DICTIONARY：1 字节 bit width + hybrid 索引。
    if (dict_texts_.empty()) {
      ret = fail("列 '%s' 使用了字典编码但缺少字典页", leaf.name.c_str());
    } else if (pos >= n) {
      ret = fail("列 '%s' 的字典索引数据为空", leaf.name.c_str());
    } else {
      const int bit_width = p[pos++];
      std::vector<uint32_t> indices;
      if (!decode_rle_hybrid(p + pos, n - pos, bit_width, count, indices)) {
        ret = fail("列 '%s' 的字典索引解码失败", leaf.name.c_str());
      } else {
        texts.reserve(count);
        for (size_t i = 0; 0 == ret && i < count; ++i) {
          if (indices[i] >= dict_texts_.size()) {
            ret = fail("列 '%s' 的字典索引 %u 越界（字典大小 %zu）",
                       leaf.name.c_str(), indices[i], dict_texts_.size());
          } else {
            texts.push_back(dict_texts_[indices[i]]);
          }
        }
      }
    }
  } else {
    ret = fail("列 '%s' 使用了不支持的编码 %d", leaf.name.c_str(), encoding);
  }
  return ret;
}

int ParquetFileReader::decode_data_page_v1(const LeafColumn &leaf, const PageHeaderInfo &ph,
                                           const std::vector<uint8_t> &page,
                                           int64_t row_count, std::vector<FileCell> &cells)
{
  int ret = 0;
  if (ph.num_values < 0) {
    return fail("列 '%s' 的数据页 num_values 非法", leaf.name.c_str());
  }
  const uint8_t *p = page.data();
  const size_t n = page.size();
  size_t pos = 0;
  if (cells.size() + static_cast<size_t>(ph.num_values) > static_cast<size_t>(row_count)) {
    return fail("列 '%s' 的数据页值数量超出 row group 行数", leaf.name.c_str());
  }
  std::vector<uint32_t> defs;
  if (leaf.max_def_level > 0) {
    int bw = 1;
    while ((1 << bw) <= leaf.max_def_level) {
      ++bw;
    }
    if (3 == ph.def_encoding) {  // RLE：4 字节长度前缀 + hybrid 数据
      if (n - pos < 4) {
        return fail("列 '%s' 的 definition levels 长度前缀缺失", leaf.name.c_str());
      }
      const uint32_t len = load_le32(p + pos);
      pos += 4;
      if (static_cast<uint64_t>(len) > n - pos) {
        return fail("列 '%s' 的 definition levels 越界", leaf.name.c_str());
      }
      if (!decode_rle_hybrid(p + pos, len, bw, static_cast<size_t>(ph.num_values), defs)) {
        return fail("列 '%s' 的 definition levels(RLE) 解码失败", leaf.name.c_str());
      }
      pos += len;
    } else if (4 == ph.def_encoding) {  // 老式 BIT_PACKED
      const size_t bytes = (static_cast<size_t>(ph.num_values) * static_cast<size_t>(bw) + 7) / 8;
      if (bytes > n - pos) {
        return fail("列 '%s' 的 definition levels 越界", leaf.name.c_str());
      }
      if (!decode_bit_packed_levels(p + pos, bytes, bw,
                                    static_cast<size_t>(ph.num_values), defs)) {
        return fail("列 '%s' 的 definition levels(BIT_PACKED) 解码失败",
                    leaf.name.c_str());
      }
      pos += bytes;
    } else {
      return fail("列 '%s' 的 definition levels 编码 %d 不支持",
                  leaf.name.c_str(), ph.def_encoding);
    }
  } else {
    defs.assign(static_cast<size_t>(ph.num_values), 0);
  }
  size_t non_null = 0;
  for (size_t i = 0; i < defs.size(); ++i) {
    if (static_cast<int32_t>(defs[i]) == leaf.max_def_level) {
      ++non_null;
    }
  }
  std::vector<std::string> texts;
  if (0 != (ret = decode_values(leaf, ph.encoding, p, n, pos, non_null, texts))) {
    return ret;
  }
  size_t ti = 0;
  for (size_t i = 0; i < defs.size(); ++i) {
    FileCell cell;
    if (static_cast<int32_t>(defs[i]) == leaf.max_def_level && ti < texts.size()) {
      cell.is_null_ = false;
      cell.text_.swap(texts[ti++]);
    }
    cells.push_back(std::move(cell));
  }
  return ret;
}

int ParquetFileReader::decode_data_page_v2(const LeafColumn &leaf, const ColumnMeta &cm,
                                           const PageHeaderInfo &ph,
                                           const std::vector<uint8_t> &raw,
                                           int64_t row_count, std::vector<FileCell> &cells)
{
  int ret = 0;
  if (ph.num_values < 0 || ph.v2_num_nulls < 0 || ph.v2_def_len < 0 || ph.v2_rep_len < 0) {
    return fail("列 '%s' 的 DataPageV2 头字段非法", leaf.name.c_str());
  }
  if (cells.size() + static_cast<size_t>(ph.num_values) > static_cast<size_t>(row_count)) {
    return fail("列 '%s' 的数据页值数量超出 row group 行数", leaf.name.c_str());
  }
  if (leaf.max_rep_level > 0 && ph.v2_rep_len > 0) {
    return fail("列 '%s' 是 REPEATED 列，暂不支持", leaf.name.c_str());
  }
  const size_t rep_len = static_cast<size_t>(ph.v2_rep_len);
  const size_t def_len = static_cast<size_t>(ph.v2_def_len);
  if (rep_len + def_len > raw.size()) {
    return fail("列 '%s' 的 DataPageV2 levels 越界", leaf.name.c_str());
  }
  // V2 的 levels 不再带 4 字节长度前缀，长度直接来自页头。
  std::vector<uint32_t> defs;
  if (leaf.max_def_level > 0) {
    if (0 == def_len) {
      return fail("列 '%s' 的 DataPageV2 definition levels 为空", leaf.name.c_str());
    }
    int bw = 1;
    while ((1 << bw) <= leaf.max_def_level) {
      ++bw;
    }
    if (!decode_rle_hybrid(raw.data() + rep_len, def_len, bw,
                           static_cast<size_t>(ph.num_values), defs)) {
      return fail("列 '%s' 的 DataPageV2 definition levels 解码失败", leaf.name.c_str());
    }
  } else {
    defs.assign(static_cast<size_t>(ph.num_values), 0);
  }
  // values 部分：可能被压缩（is_compressed 默认为 true）。
  const uint8_t *values = raw.data() + rep_len + def_len;
  size_t values_len = raw.size() - rep_len - def_len;
  const size_t expected_values =
      static_cast<size_t>(ph.uncompressed_size) - rep_len - def_len;
  std::vector<uint8_t> plain;
  if (ph.v2_is_compressed && 0 != cm.codec) {
    if (0 != (ret = decompress_page(cm.codec, values, values_len, expected_values,
                                    "DataPageV2 数据", plain))) {
      return ret;
    }
    values = plain.data();
    values_len = plain.size();
  }
  size_t non_null = static_cast<size_t>(ph.num_values) -
                    static_cast<size_t>(ph.v2_num_nulls);
  size_t vpos = 0;
  std::vector<std::string> texts;
  if (0 != (ret = decode_values(leaf, ph.encoding, values, values_len, vpos,
                                non_null, texts))) {
    return ret;
  }
  size_t ti = 0;
  for (size_t i = 0; i < defs.size(); ++i) {
    FileCell cell;
    if (static_cast<int32_t>(defs[i]) == leaf.max_def_level && ti < texts.size()) {
      cell.is_null_ = false;
      cell.text_.swap(texts[ti++]);
    }
    cells.push_back(std::move(cell));
  }
  return ret;
}

int ParquetFileReader::load_row_group(size_t rg_index)
{
  int ret = 0;
  if (rg_index >= meta_.row_groups.size()) {
    return fail("row group 下标 %zu 越界", rg_index);
  }
  const RowGroupMeta &rg = meta_.row_groups[rg_index];
  if (rg.num_rows < 0) {
    return fail("row group %zu 的 num_rows 非法", rg_index);
  }
  if (rg.columns.size() != leaves_.size()) {
    return fail("row group %zu 的列数 %zu 与 schema 叶子列数 %zu 不一致",
                rg_index, rg.columns.size(), leaves_.size());
  }
  rg_rows_.clear();
  rg_rows_.resize(static_cast<size_t>(rg.num_rows));
  for (size_t r = 0; r < rg_rows_.size(); ++r) {
    rg_rows_[r].cells_.resize(leaves_.size());
  }
  for (size_t ci = 0; 0 == ret && ci < rg.columns.size(); ++ci) {
    const ColumnMeta &cm = rg.columns[ci];
    std::string key;
    for (size_t i = 0; i < cm.path.size(); ++i) {
      if (i > 0) {
        key += '.';
      }
      key += cm.path[i];
    }
    std::map<std::string, size_t>::const_iterator it = path_to_leaf_.find(key);
    if (path_to_leaf_.end() == it) {
      ret = fail("row group %zu 的列 '%s' 不在 schema 中", rg_index, key.c_str());
      break;
    }
    const size_t leaf_index = it->second;
    std::vector<FileCell> cells;
    if (0 != (ret = decode_column(cm, leaves_[leaf_index], rg.num_rows, cells))) {
      break;
    }
    for (size_t r = 0; r < cells.size(); ++r) {
      rg_rows_[r].cells_[leaf_index] = std::move(cells[r]);
    }
  }
  if (0 == ret) {
    rg_row_index_ = 0;
  }
  return ret;
}

int ParquetFileReader::next_batch(int64_t max_rows, std::vector<FileRow> &rows, bool &eof)
{
  int ret = 0;
  rows.clear();
  eof = false;
  if (!opened_) {
    return fail("parquet 读取器尚未 open");
  }
  if (max_rows <= 0) {
    eof = (rg_row_index_ >= rg_rows_.size() && next_rg_ >= meta_.row_groups.size());
    return 0;
  }
  while (static_cast<int64_t>(rows.size()) < max_rows) {
    if (rg_row_index_ >= rg_rows_.size()) {
      rg_rows_.clear();
      rg_row_index_ = 0;
      if (next_rg_ >= meta_.row_groups.size()) {
        eof = true;
        break;
      }
      if (0 != (ret = load_row_group(next_rg_))) {
        break;
      }
      ++next_rg_;
      if (rg_rows_.empty()) {
        continue;  // 空的 row group，继续读下一个
      }
    }
    const int64_t remain = static_cast<int64_t>(rg_rows_.size() - rg_row_index_);
    const int64_t want = max_rows - static_cast<int64_t>(rows.size());
    const int64_t take = remain < want ? remain : want;
    for (int64_t i = 0; i < take; ++i) {
      rows.push_back(std::move(rg_rows_[rg_row_index_]));
      ++rg_row_index_;
    }
  }
  return ret;
}

void ParquetFileReader::close()
{
  if (in_.is_open()) {
    in_.close();
  }
  in_.clear();
  file_size_ = 0;
  meta_ = FileMeta();
  schema_.clear();
  leaves_.clear();
  path_to_leaf_.clear();
  rg_rows_.clear();
  rg_row_index_ = 0;
  next_rg_ = 0;
  opened_ = false;
  dict_texts_.clear();
}

}  // namespace

// ---------------------------------------------------------------------------
// 七、工厂函数
//
// 语义：创建对象并立即 open()（open 可以重复调用）。失败时返回 -1，且 reader
// 仍然持有对象，调用方可以通过 reader->error() 取到具体原因。
// ---------------------------------------------------------------------------
int create_parquet_file_reader(const std::string &path, const FileReadOptions &opt,
                               std::unique_ptr<FileFormatReader> &reader)
{
  (void)opt;
  int ret = 0;
  reader.reset();
  ParquetFileReader *impl = new (std::nothrow) ParquetFileReader(path);
  if (NULL == impl) {
    ret = -1;
  } else {
    std::unique_ptr<FileFormatReader> guard(impl);
    ret = impl->open();
    reader = std::move(guard);
  }
  return ret;
}

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
