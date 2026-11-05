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

// "File as database": resolve read_csv() / read_json() / read_ndjson() /
// read_parquet() / glob() used in the FROM clause into a TableItem of type
// FILE_TABLE. The column list is inferred from the local file at resolve time
// so that the rest of the engine (optimizer, executor, joins, aggregates)
// works on a normal table-like item.

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/engine/basic/ob_file_format_reader.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/ob_errno.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{

// Maps the inferred logical type of the file layer to the seekdb object type.
ObObjType file_logical_type_to_obj_type(file_sql::FileLogicalType type)
{
  ObObjType obj_type = ObVarcharType;
  switch (type) {
    case file_sql::FILE_TYPE_BOOLEAN: obj_type = ObTinyIntType; break;
    case file_sql::FILE_TYPE_BIGINT: obj_type = ObIntType; break;
    case file_sql::FILE_TYPE_DOUBLE: obj_type = ObDoubleType; break;
    case file_sql::FILE_TYPE_DATE: obj_type = ObDateType; break;
    case file_sql::FILE_TYPE_DATETIME: obj_type = ObDateTimeType; break;
    case file_sql::FILE_TYPE_VARCHAR: obj_type = ObVarcharType; break;
    case file_sql::FILE_TYPE_NULL:
    default: obj_type = ObVarcharType; break;
  }
  return obj_type;
}

// Returns the text of a string literal parse node, false if not a literal.
bool get_string_literal(const ParseNode *node, ObString &value)
{
  bool found = false;
  if (NULL != node && (T_VARCHAR == node->type_ || T_CHAR == node->type_)
      && NULL != node->str_value_) {
    value.assign_ptr(node->str_value_, static_cast<int32_t>(node->str_len_));
    found = true;
  }
  return found;
}

bool get_bool_literal(const ParseNode *node, bool &value)
{
  bool found = false;
  if (NULL != node) {
    if (T_BOOL == node->type_) {
      value = (1 == node->value_);
      found = true;
    } else if (T_NUMBER == node->type_) {
      value = (0 != node->value_);
      found = true;
    }
  }
  return found;
}

// DuckDB style named argument: `name = value`.
bool get_named_arg(const ParseNode *node, ObString &name, const ParseNode *&value_node)
{
  bool found = false;
  if (NULL != node && T_OP_EQ == node->type_ && 2 == node->num_child_
      && NULL != node->children_[0] && T_IDENT == node->children_[0]->type_) {
    name.assign_ptr(node->children_[0]->str_value_,
                    static_cast<int32_t>(node->children_[0]->str_len_));
    value_node = node->children_[1];
    found = true;
  }
  return found;
}

// Duplicate column names are disambiguated with the MySQL style `__N` suffix,
// e.g. two `id` columns become `id` and `id__2`.
int dedup_file_column_names(ObIArray<ObFileColumnDef> &columns, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObFileColumnDef &cur = columns.at(i);
    int64_t suffix = 1;
    bool dup = true;
    while (OB_SUCC(ret) && dup) {
      dup = false;
      for (int64_t j = 0; OB_SUCC(ret) && !dup && j < i; ++j) {
        if (0 == columns.at(j).column_name_.case_compare(cur.column_name_)) {
          dup = true;
        }
      }
      if (dup) {
        ++suffix;
        char buf[64];
        int64_t pos = 0;
        ObString base = columns.at(i).column_name_;
        // Strip a previously added suffix so that `id`, `id`, `id` becomes
        // `id`, `id__2`, `id__3`.
        int64_t base_len = base.length();
        for (int64_t k = base.length() - 1; k >= 0; --k) {
          if ('_' == base.ptr()[k]) {
            bool all_digit = true;
            for (int64_t m = k + 1; m < base.length(); ++m) {
              all_digit = all_digit && base.ptr()[m] >= '0' && base.ptr()[m] <= '9';
            }
            if (all_digit && k + 1 < base.length()) {
              base_len = k;
            }
            break;
          }
        }
        if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "%.*s__%ld",
                                    static_cast<int>(base_len), base.ptr(), suffix))) {
        } else {
          char *name_buf = static_cast<char *>(allocator.alloc(pos));
          if (NULL == name_buf) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(name_buf, buf, pos);
            cur.column_name_.assign_ptr(name_buf, static_cast<int32_t>(pos));
          }
        }
      }
    }
  }
  return ret;
}


// Canonicalizes both paths and verifies that the file lives inside
// secure_file_priv. Returns OB_ERR_NO_PRIVILEGE when it escapes.
int check_file_path_in_priv(const ObString &priv, const ObString &path)
{
  int ret = OB_SUCCESS;
  char buf[PATH_MAX] = {0};
  std::string canon_path;
  std::string canon_priv;
  std::string path_str(path.ptr(), path.length());
  std::string priv_str(priv.ptr(), priv.length());
  if (NULL == ::realpath(path_str.c_str(), buf)) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else {
    canon_path.assign(buf);
    MEMSET(buf, 0, sizeof(buf));
    if (NULL == ::realpath(priv_str.c_str(), buf)) {
      canon_priv = priv_str;
    } else {
      canon_priv.assign(buf);
    }
    if (canon_priv.empty()) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else if ("/" == canon_priv || canon_path == canon_priv) {
      ret = OB_SUCCESS;
    } else if (0 == canon_path.compare(0, canon_priv.size(), canon_priv)
               && canon_path.size() > canon_priv.size()
               && '/' == canon_path[canon_priv.size()]) {
      ret = OB_SUCCESS;
    } else {
      // the file must also be a regular file
      ret = OB_ERR_NO_PRIVILEGE;
    }
  }
  return ret;
}

}  // namespace

int ObDMLResolver::resolve_file_table_item(const ParseNode &parse_tree,
                                           const ParseNode *alias_node,
                                           TableItem *&tbl_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  TableItem *item = NULL;
  ObFileTableDef *table_def = NULL;
  ObString func_name;
  const ParseNode *args = NULL;

  if (OB_ISNULL(stmt) || OB_ISNULL(allocator_) || OB_ISNULL(session_info_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(T_FILE_TABLE_EXPRESSION != parse_tree.type_)
             || OB_UNLIKELY(parse_tree.num_child_ < 2)
             || OB_ISNULL(parse_tree.children_[0]) || OB_ISNULL(parse_tree.children_[1])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected file table parse node", K(ret), K(parse_tree.type_), K(parse_tree.num_child_));
  } else {
    func_name.assign_ptr(parse_tree.children_[0]->str_value_,
                         static_cast<int32_t>(parse_tree.children_[0]->str_len_));
    args = parse_tree.children_[1];
    if (NULL == alias_node && parse_tree.num_child_ >= 3) {
      alias_node = parse_tree.children_[2];
    }
  }

  // Parse the function name into a format / function type.
  int64_t func_type = -1;
  if (OB_FAIL(ret)) {
  } else if (0 == func_name.case_compare("read_csv")) {
    func_type = ObFileTableDef::READ_CSV;
  } else if (0 == func_name.case_compare("read_json")) {
    func_type = ObFileTableDef::READ_JSON;
  } else if (0 == func_name.case_compare("read_ndjson")) {
    func_type = ObFileTableDef::READ_NDJSON;
  } else if (0 == func_name.case_compare("read_parquet")) {
    func_type = ObFileTableDef::READ_PARQUET;
  } else if (0 == func_name.case_compare("glob")) {
    func_type = ObFileTableDef::GLOB_FUNC;
  } else {
    ret = OB_ERR_PARSER_SYNTAX;
    LOG_WARN("unknown file table function", K(ret), K(func_name));
  }

  if (OB_SUCC(ret)) {
    char *def_buf = static_cast<char *>(allocator_->alloc(sizeof(ObFileTableDef)));
    if (OB_ISNULL(def_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      table_def = new (def_buf) ObFileTableDef();
      table_def->func_type_ = func_type;
    }
  }

  // The first argument must be a string literal file path, the remaining
  // arguments must be named (DuckDB style `name = value`).
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(args) || OB_UNLIKELY(T_EXPR_LIST != args->type_) || 0 == args->num_child_) {
      ret = OB_ERR_PARSER_SYNTAX;
      LOG_WARN("file table function requires a path argument", K(ret));
    } else if (OB_FAIL(get_string_literal(args->children_[0], table_def->file_path_))) {
      ret = OB_ERR_PARSER_SYNTAX;
      LOG_WARN("the first argument of the file table function must be a string literal", K(ret));
    }
    for (int64_t i = 1; OB_SUCC(ret) && i < args->num_child_; ++i) {
      ObString arg_name;
      const ParseNode *value_node = NULL;
      if (OB_FAIL(get_named_arg(args->children_[i], arg_name, value_node))) {
        ret = OB_ERR_PARSER_SYNTAX;
        LOG_WARN("expected a named argument like name = value", K(ret), K(i));
      } else if (0 == arg_name.case_compare("header")) {
        if (OB_FAIL(get_bool_literal(value_node, table_def->header_))) {
          ret = OB_ERR_PARSER_SYNTAX;
        }
      } else if (0 == arg_name.case_compare("sample_size")) {
        // Only a full file scan is supported by design.
        bool is_all = false;
        if (NULL != value_node && T_NUMBER == value_node->type_ && -1 == value_node->value_) {
          is_all = true;
        } else if (NULL != value_node && T_OP_NEG == value_node->type_
                   && 1 == value_node->num_child_ && NULL != value_node->children_[0]
                   && T_NUMBER == value_node->children_[0]->type_
                   && 1 == value_node->children_[0]->value_) {
          is_all = true;
        }
        if (!is_all) {
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("only sample_size = -1 is supported", K(ret));
        }
      } else if (0 == arg_name.case_compare("format")) {
        if (OB_FAIL(get_string_literal(value_node, table_def->format_))) {
          ret = OB_ERR_PARSER_SYNTAX;
        } else if (0 == table_def->format_.case_compare("csv")) {
          // ok
        } else if (0 == table_def->format_.case_compare("newline_delimited")
                   || 0 == table_def->format_.case_compare("ndjson")
                   || 0 == table_def->format_.case_compare("jsonl")) {
          table_def->func_type_ = ObFileTableDef::READ_JSON;
        } else {
          ret = OB_ERR_PARSER_SYNTAX;
          LOG_WARN("unsupported file format", K(ret), K(table_def->format_));
        }
      } else if (0 == arg_name.case_compare("delimiter")) {
        ObString value;
        if (OB_FAIL(get_string_literal(value_node, value)) || 1 != value.length()) {
          ret = OB_ERR_PARSER_SYNTAX;
        }
      } else {
        ret = OB_ERR_PARSER_SYNTAX;
        LOG_WARN("unknown named argument", K(ret), K(arg_name));
      }
    }
  }

  // Secure path check: the resolved file must be inside secure_file_priv.
  if (OB_SUCC(ret) && ObFileTableDef::GLOB_FUNC != table_def->func_type_) {
    ObString secure_path;
    ObObj global_val;
    if (OB_FAIL(session_info_->get_secure_file_priv(secure_path)) || secure_path.empty()) {
      // secure_file_priv is a GLOBAL variable, the session level value may be
      // empty, fall back to the global one.
      ret = OB_SUCCESS;
      secure_path.reset();
      if (OB_FAIL(ObBasicSessionInfo::get_global_sys_variable(
                    session_info_, *allocator_,
                    ObString::make_string("secure_file_priv"), global_val))) {
      } else if (OB_FAIL(global_val.get_string(secure_path))) {
      }
    }
    if (OB_SUCC(ret)
        && OB_FAIL(check_file_path_in_priv(secure_path, table_def->file_path_))) {
    }
  }

  // Create the table item.
  if (OB_SUCC(ret)) {
    ObString table_name;
    if (OB_ISNULL(alias_node)) {
      table_name = func_name;
    } else if (FALSE_IT(table_name.assign_ptr(alias_node->str_value_,
                                             static_cast<int32_t>(alias_node->str_len_)))) {
    } else if (table_name.empty()) {
      table_name = func_name;
    }
    if (OB_ISNULL(item = stmt->create_table_item(*allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      item->table_name_ = table_name;
      item->alias_name_ = table_name;
      item->table_id_ = generate_table_id();
      item->type_ = TableItem::FILE_TABLE;
      item->file_table_def_ = table_def;
      tbl_item = item;
    }
  }

  // Infer the schema of the file with the shared format layer.
  if (OB_SUCC(ret)) {
    file_sql::FileReadOptions options;
    switch (table_def->func_type_) {
      case ObFileTableDef::READ_JSON:
      case ObFileTableDef::READ_NDJSON:
        options.format_ = file_sql::FILE_FORMAT_JSONL;
        break;
      case ObFileTableDef::READ_PARQUET:
        options.format_ = file_sql::FILE_FORMAT_PARQUET;
        break;
      default:
        options.format_ = file_sql::FILE_FORMAT_CSV;
        break;
    }
    options.header_ = table_def->header_;
    options.sample_size_ = table_def->sample_size_;
    std::string path(table_def->file_path_.ptr(), table_def->file_path_.length());
    std::unique_ptr<file_sql::FileFormatReader> reader;
    std::vector<file_sql::FileColumnSchema> schema;
    if (ObFileTableDef::GLOB_FUNC == table_def->func_type_) {
      ObFileColumnDef col;
      col.column_name_ = ObString::make_string("file");
      col.obj_type_ = ObVarcharType;
      col.nullable_ = false;
      if (OB_FAIL(table_def->columns_.push_back(col))) {
      }
    } else if (0 != file_sql::create_file_reader(path, options, reader) || NULL == reader.get()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to open file for schema inference", K(ret), K(path.c_str()));
    } else if (0 != reader->open()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to infer file schema", K(ret), K(path.c_str()),
               K(reader->error().c_str()));
    } else {
      schema = reader->get_schema();
      for (size_t i = 0; OB_SUCC(ret) && i < schema.size(); ++i) {
        ObFileColumnDef col;
        if (OB_FAIL(ob_write_string(*allocator_,
                                    ObString(static_cast<int32_t>(schema[i].name_.size()),
                                             schema[i].name_.data()),
                                    col.column_name_))) {
        } else {
          col.obj_type_ = file_logical_type_to_obj_type(schema[i].type_);
          col.nullable_ = schema[i].nullable_;
          if (OB_FAIL(table_def->columns_.push_back(col))) {
          }
        }
      }
      reader->close();
    }
  }

  if (OB_SUCC(ret) && OB_FAIL(dedup_file_column_names(table_def->columns_, *allocator_))) {
  }

  // Build one ColumnItem per inferred column.
  for (int64_t i = 0; OB_SUCC(ret) && i < table_def->columns_.count(); ++i) {
    const ObFileColumnDef &col = table_def->columns_.at(i);
    ObColumnRefRawExpr *col_expr = NULL;
    ColumnItem column_item;
    sql::ObRawExprResType result_type;
    ObDataType data_type;
    data_type.set_obj_type(col.obj_type_);
    data_type.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    data_type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[col.obj_type_]);
    if (OB_ISNULL(params_.expr_factory_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_REF_COLUMN, col_expr))) {
    } else if (OB_ISNULL(col_expr)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      col_expr->set_ref_id(item->table_id_, OB_APP_MIN_COLUMN_ID + i);
      result_type.set_meta(data_type.get_meta_type());
      result_type.set_accuracy(data_type.get_accuracy());
      col_expr->set_result_type(result_type);
      col_expr->set_column_attr(item->get_object_name(), col.column_name_);
      col_expr->set_database_name(item->database_name_);
      if (col.nullable_) {
        col_expr->unset_result_flag(NOT_NULL_FLAG);
      } else {
        col_expr->set_result_flag(NOT_NULL_FLAG);
      }
      column_item.expr_ = col_expr;
      column_item.table_id_ = col_expr->get_table_id();
      column_item.column_id_ = col_expr->get_column_id();
      column_item.column_name_ = col_expr->get_column_name();
      if (OB_FAIL(col_expr->extract_info())) {
      } else if (OB_FAIL(stmt->add_column_item(column_item))) {
      }
    }
  }

  if (OB_SUCC(ret) && OB_FAIL(stmt->add_table_item(session_info_, item))) {
  }
  return ret;
}

int ObSelectResolver::resolve_all_file_table_columns(const TableItem &table_item,
                                                     ObIArray<ColumnItem> *column_items)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  if (OB_ISNULL(column_items) || OB_ISNULL(stmt) || OB_ISNULL(table_item.file_table_def_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret)
         && i < table_item.file_table_def_->columns_.count(); ++i) {
      ColumnItem *col_item = stmt->get_column_item_by_id(
          table_item.table_id_, OB_APP_MIN_COLUMN_ID + i);
      if (OB_ISNULL(col_item)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("file table column item not found", K(ret), K(i));
      } else if (OB_FAIL(column_items->push_back(*col_item))) {
      }
    }
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
