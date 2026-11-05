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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "share/ob_errno.h"
#include "lib/oblog/ob_log_module.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFileTableSpec, ObOpSpec), column_exprs_, column_idxs_,
                    file_path_, format_, header_);

namespace
{

// Days since 1970-01-01 of a proleptic Gregorian date.
int64_t civil_to_days(int64_t y, int64_t m, int64_t d)
{
  y -= (m <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

bool parse_four(const char *p, int64_t &v)
{
  bool ok = p[0] >= '0' && p[0] <= '9'
         && p[1] >= '0' && p[1] <= '9'
         && p[2] >= '0' && p[2] <= '9'
         && p[3] >= '0' && p[3] <= '9';
  if (ok) {
    v = (p[0] - '0') * 1000 + (p[1] - '0') * 100
      + (p[2] - '0') * 10 + (p[3] - '0');
  }
  return ok;
}

bool parse_two(const char *p, int64_t &v)
{
  bool ok = p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9';
  if (ok) {
    v = (p[0] - '0') * 10 + (p[1] - '0');
  }
  return ok;
}

}  // namespace

ObFileTableOp::ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
  : ObOperator(exec_ctx, spec, input),
    reader_(nullptr),
    batch_(),
    batch_pos_(0),
    eof_(false),
    inited_(false),
    row_alloc_()
{
}

int ObFileTableOp::open_reader()
{
  int ret = OB_SUCCESS;
  int reader_ret = 0;
  const ObFileTableSpec &spec = static_cast<const ObFileTableSpec &>(spec_);
  file_sql::FileReadOptions options;
  options.format_ = static_cast<file_sql::FileFormat>(spec.format_);
  options.header_ = spec.header_;
  std::string path(spec.file_path_.ptr(), spec.file_path_.length());
  if (0 != (reader_ret = file_sql::create_file_reader(path, options, reader_))
      || nullptr == reader_.get()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to create file reader", K(ret), K(reader_ret), K(path.c_str()),
             K(spec.format_), K(spec.column_exprs_.count()));
  } else if (0 != (reader_ret = reader_->open())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to open file reader", K(ret), K(reader_ret), K(path.c_str()),
             K(spec.format_), K(reader_->error().c_str()));
  } else if (spec.column_exprs_.count() != spec.column_idxs_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file table expression and column index counts differ", K(ret),
             K(spec.column_exprs_.count()), K(spec.column_idxs_.count()));
  } else {
    const int64_t schema_count = static_cast<int64_t>(reader_->get_schema().size());
    for (int64_t i = 0; OB_SUCC(ret) && i < spec.column_idxs_.count(); ++i) {
      if (spec.column_idxs_.at(i) < 0 || spec.column_idxs_.at(i) >= schema_count) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("file table column index is outside reader schema", K(ret), K(path.c_str()),
                 K(i), K(spec.column_idxs_.at(i)), K(schema_count));
      }
    }
  }
  batch_.clear();
  batch_pos_ = 0;
  eof_ = false;
  return ret;
}

int ObFileTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(open_reader())) {
  } else {
    inited_ = true;
  }
  return ret;
}

int ObFileTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  reader_.reset();
  row_alloc_.reset();
  if (OB_FAIL(open_reader())) {
  }
  return ret;
}

void ObFileTableOp::destroy()
{
  if (nullptr != reader_.get()) {
    reader_->close();
    reader_.reset();
  }
  batch_.clear();
  ObOperator::destroy();
}

int ObFileTableOp::set_cell_datum(ObExpr *expr, int64_t column_type, const file_sql::FileCell &cell)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
    if (cell.is_null_) {
      datum.set_null();
    } else if (0 == cell.text_.compare("")) {
      datum.set_null();
    } else {
      const char *p = cell.text_.c_str();
      switch (column_type) {
        case file_sql::FILE_TYPE_BOOLEAN: {
          std::string lower = cell.text_;
          for (size_t k = 0; k < lower.size(); ++k) {
            lower[k] = static_cast<char>(::tolower(static_cast<unsigned char>(lower[k])));
          }
          const bool v = (0 == lower.compare("true") || 0 == lower.compare("1"));
          datum.set_int(v ? 1 : 0);
          break;
        }
        case file_sql::FILE_TYPE_BIGINT: {
          errno = 0;
          char *end = nullptr;
          const long long v = strtoll(p, &end, 10);
          if (0 == errno && nullptr != end && '\0' == *end) {
            datum.set_int(static_cast<int64_t>(v));
          } else {
            datum.set_null();
          }
          break;
        }
        case file_sql::FILE_TYPE_DOUBLE: {
          char *end = nullptr;
          const double v = strtod(p, &end);
          if (nullptr != end && '\0' == *end) {
            datum.set_double(v);
          } else {
            datum.set_null();
          }
          break;
        }
        case file_sql::FILE_TYPE_DATE: {
          int64_t y = 0, m = 0, d = 0;
          const int64_t len = static_cast<int64_t>(cell.text_.size());
          if (len >= 10 && parse_four(p, y) && parse_two(p + 5, m) && parse_two(p + 8, d)
              && '-' == p[4] && '-' == p[7]) {
            datum.set_date(static_cast<int32_t>(civil_to_days(y, m, d)));
          } else {
            datum.set_null();
          }
          break;
        }
        case file_sql::FILE_TYPE_DATETIME: {
          int64_t y = 0, m = 0, d = 0, hh = 0, mi = 0, ss = 0;
          const int64_t len = static_cast<int64_t>(cell.text_.size());
          if (len >= 19 && '-' == p[4] && '-' == p[7]
              && (' ' == p[10] || 'T' == p[10])
              && ':' == p[13] && ':' == p[16]
              && parse_four(p, y) && parse_two(p + 5, m) && parse_two(p + 8, d)
              && parse_two(p + 11, hh) && parse_two(p + 14, mi) && parse_two(p + 17, ss)) {
            const int64_t seconds = civil_to_days(y, m, d) * 86400 + hh * 3600 + mi * 60 + ss;
            datum.set_datetime(seconds * 1000000L);
          } else {
            datum.set_null();
          }
          break;
        }
        default: {
          const int64_t len = static_cast<int64_t>(cell.text_.size());
          char *buf = static_cast<char *>(row_alloc_.alloc(len));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(buf, p, len);
            datum.set_string(buf, len);
          }
          break;
        }
      }
    }
    if (OB_SUCC(ret)) {
      expr->set_evaluated_projected(eval_ctx_);
    }
  }
  return ret;
}

int ObFileTableOp::fill_row_datums(const file_sql::FileRow &row)
{
  int ret = OB_SUCCESS;
  const ObFileTableSpec &spec = static_cast<const ObFileTableSpec &>(spec_);
  if (spec.column_exprs_.count() != spec.column_idxs_.count()) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < spec.column_exprs_.count(); ++i) {
    const int64_t source_idx = spec.column_idxs_.at(i);
    ObExpr *expr = spec.column_exprs_.at(i);
    if (source_idx < 0 || source_idx >= static_cast<int64_t>(row.cells_.size())
        || OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid file table row projection", K(ret), K(i), K(source_idx),
               K(row.cells_.size()), KP(expr));
    } else {
      const ObObjType obj_type = expr->datum_meta_.type_;
      int64_t logical_type = file_sql::FILE_TYPE_VARCHAR;
      switch (obj_type) {
        case ObTinyIntType: logical_type = file_sql::FILE_TYPE_BOOLEAN; break;
        case ObIntType: logical_type = file_sql::FILE_TYPE_BIGINT; break;
        case ObDoubleType: logical_type = file_sql::FILE_TYPE_DOUBLE; break;
        case ObDateType: logical_type = file_sql::FILE_TYPE_DATE; break;
        case ObDateTimeType: logical_type = file_sql::FILE_TYPE_DATETIME; break;
        default: logical_type = file_sql::FILE_TYPE_VARCHAR; break;
      }
      if (OB_FAIL(set_cell_datum(expr, logical_type, row.cells_.at(source_idx)))) {
      }
    }
  }
  return ret;
}

int ObFileTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  while (OB_SUCC(ret) && (batch_pos_ >= static_cast<int64_t>(batch_.size()))) {
    batch_.clear();
    batch_pos_ = 0;
    bool eof = false;
    if (nullptr == reader_.get()) {
      ret = OB_NOT_INIT;
    } else if (0 != reader_->next_batch(BATCH_SIZE, batch_, eof)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to read next file batch", K(ret), K(reader_->error().c_str()));
    } else {
      eof_ = eof;
      if (batch_.empty()) {
        ret = OB_ITER_END;
      }
    }
  }
  if (OB_SUCC(ret)) {
    row_alloc_.reset();
    const file_sql::FileRow &row = batch_.at(batch_pos_++);
    if (OB_FAIL(fill_row_datums(row))) {
    }
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
