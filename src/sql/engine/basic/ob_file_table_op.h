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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_OP_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_format_reader.h"
#include <memory>
#include <vector>

namespace oceanbase
{
namespace sql
{

// Leaf operator of the "file as database" feature: it owns a streaming reader
// of the local file and turns its text cells into datums of the output
// columns. Rows are produced in batches, the whole file is never materialized.
class ObFileTableSpec : public ObOpSpec
{
public:
  ObFileTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      column_exprs_(alloc),
      file_path_(),
      format_(0),
      header_(true) {}
  // one expression per inferred column, filled by the code generator
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
  common::ObString file_path_;
  int64_t format_;
  bool header_;
};

class ObFileTableOp : public ObOperator
{
public:
  ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input);
  virtual ~ObFileTableOp() {}
  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual void destroy() override;
  virtual int inner_get_next_row() override;

private:
  int open_reader();
  int fill_row_datums(const file_sql::FileRow &row);
  int set_cell_datum(ObExpr *expr, int64_t column_type, const file_sql::FileCell &cell);
  static const int64_t BATCH_SIZE = 256;

private:
  std::unique_ptr<file_sql::FileFormatReader> reader_;
  std::vector<file_sql::FileRow> batch_;
  int64_t batch_pos_;
  bool eof_;
  bool inited_;
  common::ObArenaAllocator row_alloc_;
};

}  // namespace sql
}  // namespace oceanbase

#endif  // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_OP_H_
