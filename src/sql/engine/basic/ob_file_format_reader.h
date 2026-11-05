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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_FORMAT_READER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_FORMAT_READER_H_

// Self-contained local-file format layer used by the "file as database"
// feature (read_csv / read_json / read_ndjson / read_parquet / glob).
// It is deliberately written in plain C++ so that it can be compiled and
// unit-tested on its own, see ob_file_format_reader.cpp.

#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

// Logical types produced by deterministic type inference.
// The order matters: wider types have larger values, so widening is
// simply "max(type of value)".
enum FileLogicalType : int32_t
{
  FILE_TYPE_NULL = 0,   // only NULL values seen
  FILE_TYPE_BOOLEAN = 1,
  FILE_TYPE_BIGINT = 2,
  FILE_TYPE_DOUBLE = 3,
  FILE_TYPE_DATE = 4,
  FILE_TYPE_DATETIME = 5,
  FILE_TYPE_VARCHAR = 6,
};

enum FileFormat : int32_t
{
  FILE_FORMAT_CSV = 0,
  FILE_FORMAT_JSONL = 1,
  FILE_FORMAT_PARQUET = 2,
  FILE_FORMAT_GLOB = 3,
};

// CSV / JSONL / Parquet read options. Values are the ones parsed from the
// SQL table function arguments.
struct FileReadOptions
{
  FileReadOptions()
    : format_(FILE_FORMAT_CSV),
      header_(true),
      sample_size_(-1),
      delimiter_(','),
      quote_('"'),
      escape_('"'),
      null_str_(),
      has_header_option_(false)
  {}
  FileFormat format_;
  bool header_;
  int64_t sample_size_;      // -1: full file scan (only supported value)
  char delimiter_;
  char quote_;
  char escape_;
  std::string null_str_;     // empty string default: empty cell means NULL
  bool has_header_option_;
};

struct FileColumnSchema
{
  FileColumnSchema() : name_(), type_(FILE_TYPE_NULL), nullable_(true) {}
  FileColumnSchema(const std::string &name, FileLogicalType type, bool nullable)
    : name_(name), type_(type), nullable_(nullable)
  {}
  std::string name_;
  FileLogicalType type_;
  bool nullable_;
};

// One output cell. When is_null_ is true the other fields are ignored.
struct FileCell
{
  FileCell() : is_null_(true), text_() {}
  bool is_null_;
  std::string text_;  // canonical text representation of the value
};

struct FileRow
{
  FileRow() : cells_() {}
  std::vector<FileCell> cells_;
};

// Row producer interface. All methods return 0 on success and a negative
// value on failure; error() gives a human readable diagnostic.
class FileFormatReader
{
public:
  virtual ~FileFormatReader() {}
  // Parses the file header / footer and infers the schema. Must not
  // materialize the whole file.
  virtual int open() = 0;
  virtual const std::vector<FileColumnSchema> &get_schema() const = 0;
  // Reads at most max_rows rows. eof is set when no more rows are available.
  virtual int next_batch(int64_t max_rows, std::vector<FileRow> &rows, bool &eof) = 0;
  virtual void close() = 0;
  virtual const std::string &error() const = 0;
};

// Creates the reader for the given path / format.
//   csv     : FileReadOptions::format_ == FILE_FORMAT_CSV
//   jsonl   : FILE_FORMAT_JSONL (read_json / read_ndjson)
//   parquet : FILE_FORMAT_PARQUET
int create_file_reader(const std::string &path,
                       const FileReadOptions &options,
                       std::unique_ptr<FileFormatReader> &reader);
int create_jsonl_file_reader(const std::string &path,
                             const FileReadOptions &options,
                             std::unique_ptr<FileFormatReader> &reader);
int create_parquet_file_reader(const std::string &path,
                               const FileReadOptions &options,
                               std::unique_ptr<FileFormatReader> &reader);

// glob(): lists the regular files matching a shell style pattern, sorted by
// name. Only '*' and '?' wildcards and a literal directory are supported.
int glob_files(const std::string &pattern, std::vector<std::string> &files);

// COPY ... TO 'path' (format csv, header, nullstr 'x') support.
struct FileCopyOptions
{
  FileCopyOptions()
    : delimiter_(','),
      quote_('"'),
      escape_('"'),
      null_str_("NULL"),
      header_(true)
  {}
  char delimiter_;
  char quote_;
  char escape_;
  std::string null_str_;
  bool header_;
};

class FileCopyWriter
{
public:
  virtual ~FileCopyWriter() {}
  virtual int open() = 0;
  virtual int write_header(const std::vector<std::string> &column_names) = 0;
  virtual int write_row(const std::vector<FileCell> &cells) = 0;
  // Atomically publishes the file; fails when the target already exists.
  virtual int close() = 0;
  // Removes the temporary file, used when the query fails.
  virtual void abort() = 0;
  virtual const std::string &error() const = 0;
};

int create_file_copy_writer(const std::string &path,
                            const FileCopyOptions &options,
                            std::unique_ptr<FileCopyWriter> &writer);

// Deterministic type inference helpers, shared by all readers.
// Returns true when text can be parsed as the type without loss.
bool file_text_is_bigint(const std::string &text);
bool file_text_is_double(const std::string &text);
bool file_text_is_boolean(const std::string &text);
bool file_text_is_date(const std::string &text);
bool file_text_is_datetime(const std::string &text);
// Infers the logical type of one raw cell, FILE_TYPE_NULL for empty input.
FileLogicalType file_infer_type(const std::string &text);
// Widening rule: BIGINT + DOUBLE => DOUBLE, anything + VARCHAR => VARCHAR,
// NULL + T => T, and BOOLEAN / DATE / DATETIME mixed with anything else
// widens to VARCHAR (except with NULL).
FileLogicalType file_widen_type(FileLogicalType left, FileLogicalType right);
const char *file_logical_type_name(FileLogicalType type);

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase

#endif  // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_FORMAT_READER_H_
