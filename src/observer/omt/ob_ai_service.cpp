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

#define USING_LOG_PREFIX SERVER_OMT
#include "ob_ai_service.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "lib/string/ob_sql_string.h"
#include "lib/time/ob_time_utility.h"
#include "lib/allocator/page_arena.h"
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include <unistd.h>

using namespace oceanbase::share;
using namespace oceanbase::common;
namespace oceanbase
{
namespace omt
{

namespace
{
struct AIColumnMeta
{
  uint64_t table_id_;
  int64_t column_id_;
  int64_t max_retries_;
  ObString database_name_;
  ObString table_name_;
  ObString column_name_;
  ObString expression_;
};

int copy_string(ObIAllocator &allocator, const ObString &src, ObString &dst)
{
  return ob_write_string(allocator, src, dst);
}

// Retry backoff without blocking the whole worker: a column that just failed
// with retries left is skipped for a short while so other rows keep flowing.
const int64_t AI_DEFER_SLOTS = 64;
uint64_t g_ai_defer_keys[AI_DEFER_SLOTS];
int64_t g_ai_defer_until[AI_DEFER_SLOTS];
int64_t g_ai_defer_count = 0;

uint64_t ai_defer_key(const uint64_t table_id, const int64_t column_id)
{
  return (table_id << 20) ^ static_cast<uint64_t>(column_id);
}

bool ai_column_deferred(const uint64_t table_id, const int64_t column_id,
                        const int64_t now)
{
  const uint64_t key = ai_defer_key(table_id, column_id);
  for (int64_t i = 0; i < g_ai_defer_count; ++i) {
    if (g_ai_defer_keys[i] == key) {
      return now < g_ai_defer_until[i];
    }
  }
  return false;
}

void ai_column_defer(const uint64_t table_id, const int64_t column_id,
                     const int64_t now)
{
  const uint64_t key = ai_defer_key(table_id, column_id);
  for (int64_t i = 0; i < g_ai_defer_count; ++i) {
    if (g_ai_defer_keys[i] == key) {
      g_ai_defer_until[i] = now + 1000 * 1000; // 1s
      return;
    }
  }
  if (g_ai_defer_count < AI_DEFER_SLOTS) {
    g_ai_defer_keys[g_ai_defer_count] = key;
    g_ai_defer_until[g_ai_defer_count] = now + 1000 * 1000;
    ++g_ai_defer_count;
  }
}

bool is_space(const char c)
{
  return ' ' == c || '\t' == c || '\n' == c || '\r' == c;
}

// Split "AI_COMPLETE('model', <prompt>) /* hint */" into its parts.
int parse_ai_expression(ObIAllocator &allocator,
                        const ObString &expr,
                        bool &is_embed,
                        ObString &model,
                        ObString &prompt,
                        ObString &hint)
{
  int ret = OB_SUCCESS;
  const char *ptr = expr.ptr();
  int64_t len = expr.length();
  int64_t core_end = len;
  int64_t hint_begin = -1;
  int64_t hint_end = -1;
  for (int64_t i = len - 1; i >= 1; --i) {
    if ('/' == ptr[i] && '*' == ptr[i - 1]) {
      hint_end = i - 1;
      for (int64_t j = i - 2; j >= 1; --j) {
        if ('*' == ptr[j] && '/' == ptr[j - 1]) {
          hint_begin = j + 1;
          break;
        }
      }
      core_end = i - 1;
      break;
    }
  }
  if (hint_begin >= 0 && hint_end >= hint_begin) {
    int64_t b = hint_begin;
    int64_t e = hint_end;
    while (b < e && is_space(ptr[b])) ++b;
    while (e > b && is_space(ptr[e - 1])) --e;
    if (b < e) {
      OZ(copy_string(allocator, ObString(static_cast<int32_t>(e - b), ptr + b), hint));
    }
  }
  while (core_end > 0 && is_space(ptr[core_end - 1])) {
    --core_end;
  }
  is_embed = (core_end >= 8 && 0 == strncasecmp(ptr, "AI_EMBED", 8));
  int64_t open = -1;
  for (int64_t i = 0; i < core_end; ++i) {
    if ('(' == ptr[i]) {
      open = i;
      break;
    }
  }
  int64_t close = -1;
  for (int64_t i = core_end - 1; i > open; --i) {
    if (')' == ptr[i]) {
      close = i;
      break;
    }
  }
  if (open < 0 || close <= open) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    int64_t arg_begin[4] = {open + 1, -1, -1, -1};
    int64_t arg_end[4] = {-1, -1, -1, -1};
    int64_t arg_count = 0;
    int64_t depth = 0;
    char quote = 0;
    for (int64_t i = open + 1; OB_SUCC(ret) && i <= close; ++i) {
      const char c = (i == close) ? ',' : ptr[i];
      if (quote != 0) {
        if (c == quote && '\\' != ptr[i - 1]) {
          quote = 0;
        }
      } else if ('\'' == c || '"' == c || '`' == c) {
        quote = c;
      } else if ('(' == c) {
        ++depth;
      } else if (')' == c) {
        --depth;
      } else if (',' == c && 0 == depth) {
        if (arg_count < 4) {
          arg_end[arg_count] = i;
          ++arg_count;
          if (i < close) {
            arg_begin[arg_count] = i + 1;
          }
        }
      }
    }
    if (arg_count < 2) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      int64_t b = arg_begin[0];
      int64_t e = arg_end[0];
      while (b < e && is_space(ptr[b])) ++b;
      while (e > b && is_space(ptr[e - 1])) --e;
      if (e - b >= 2 && ('\'' == ptr[b] || '"' == ptr[b])) {
        ++b;
        --e;
      }
      OZ(copy_string(allocator, ObString(static_cast<int32_t>(e - b), ptr + b), model));
      b = arg_begin[1];
      e = arg_end[1];
      while (b < e && is_space(ptr[b])) ++b;
      while (e > b && is_space(ptr[e - 1])) --e;
      if (e <= b) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        OZ(copy_string(allocator, ObString(static_cast<int32_t>(e - b), ptr + b), prompt));
      }
    }
  }
  return ret;
}

int append_sql_literal(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  OZ(sql.append("'"));
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    if ('\\' == c) {
      OZ(sql.append("\\\\"));
    } else if ('\'' == c) {
      OZ(sql.append("''"));
    } else {
      OZ(sql.append_fmt("%c", c));
    }
  }
  OZ(sql.append("'"));
  return ret;
}

int read_two_strings(ObIAllocator &allocator,
                     const ObString &sql,
                     ObString &value,
                     ObString &hash,
                     bool &is_null)
{
  int ret = OB_SUCCESS;
  is_null = false;
  HEAP_VAR(ObMySQLProxy::ReadResult, res) {
    common::sqlclient::ObMySQLResult *result = NULL;
    ObObj first_obj;
    ObString tmp_value;
    ObString tmp_hash;
    if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result->next())) {
      if (OB_ITER_END == ret) {
        ret = OB_ENTRY_NOT_EXIST;
      }
    } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(0), first_obj, NULL))) {
    } else if (first_obj.is_null()) {
      is_null = true;
    } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(0), tmp_value))) {
    } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(1), tmp_hash))) {
    } else if (OB_FAIL(ob_write_string(allocator, tmp_value, value))) {
    } else if (OB_FAIL(ob_write_string(allocator, tmp_hash, hash))) {
    }
  }
  return ret;
}

int publish_group(const ObIArray<AIColumnMeta> &metas,
                  const ObIArray<ObString> &results,
                  const ObIArray<ObString> &hashes,
                  const ObIArray<ObString> &prompts,
                  const ObString &pk_predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  OZ(sql.append_fmt("update `%.*s`.`%.*s` set ",
      metas.at(0).database_name_.length(), metas.at(0).database_name_.ptr(),
      metas.at(0).table_name_.length(), metas.at(0).table_name_.ptr()));
  for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
    ObSqlString state_name;
    ObSqlString spec_name;
    OZ(state_name.append_fmt("__ai_state_%.*s",
        metas.at(i).column_name_.length(), metas.at(i).column_name_.ptr()));
    OZ(spec_name.append_fmt("__ai_spec_%.*s",
        metas.at(i).column_name_.length(), metas.at(i).column_name_.ptr()));
    if (i > 0) {
      OZ(sql.append(", "));
    }
    OZ(sql.append_fmt("`%.*s` = ",
        metas.at(i).column_name_.length(), metas.at(i).column_name_.ptr()));
    OZ(append_sql_literal(sql, results.at(i)));
    OZ(sql.append_fmt(", `%.*s` = 2, `%.*s` = ",
        state_name.length(), state_name.ptr(),
        spec_name.length(), spec_name.ptr()));
    OZ(append_sql_literal(sql, hashes.at(i)));
  }
  OZ(sql.append_fmt(" where %.*s", pk_predicate.length(), pk_predicate.ptr()));
  for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
    ObSqlString state_name;
    OZ(state_name.append_fmt("__ai_state_%.*s",
        metas.at(i).column_name_.length(), metas.at(i).column_name_.ptr()));
    OZ(sql.append_fmt(" and (`%.*s` is null or `%.*s` in (0, 3))",
        state_name.length(), state_name.ptr(),
        state_name.length(), state_name.ptr()));
    OZ(sql.append_fmt(" and md5(%.*s) = ", prompts.at(i).length(),
        prompts.at(i).ptr()));
    OZ(append_sql_literal(sql, hashes.at(i)));
  }
  if (OB_SUCC(ret)) {
    OZ(GCTX.sql_proxy_->write(sql.ptr(), affected_rows));
  }
  return ret;
}

int mark_failure(const AIColumnMeta &meta,
                 const ObString &pk_predicate,
                 const int64_t retry_count){
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSqlString state_name;
  ObSqlString request_name;
  int64_t affected_rows = 0;
  const int64_t next_retry = retry_count + 1;
  const int64_t next_state = (next_retry > meta.max_retries_) ? 4 : 3;
  OZ(state_name.append_fmt("__ai_state_%.*s",
      meta.column_name_.length(), meta.column_name_.ptr()));
  OZ(request_name.append_fmt("__ai_request_%.*s",
      meta.column_name_.length(), meta.column_name_.ptr()));
  OZ(sql.append_fmt("update `%.*s`.`%.*s` set `%.*s` = %ld, `%.*s` = '%ld' "
      "where %.*s and (`%.*s` is null or `%.*s` in (0, 3))",
      meta.database_name_.length(), meta.database_name_.ptr(),
      meta.table_name_.length(), meta.table_name_.ptr(),
      state_name.length(), state_name.ptr(), next_state,
      request_name.length(), request_name.ptr(), next_retry,
      pk_predicate.length(), pk_predicate.ptr(),
      state_name.length(), state_name.ptr(),
      state_name.length(), state_name.ptr()));
  if (OB_SUCC(ret)) {
    OZ(GCTX.sql_proxy_->write(sql.ptr(), affected_rows));
  }
  return ret;
}

void handle_failure(const AIColumnMeta &meta,
                    const ObString &pk_predicate,
                    const int64_t retry_count)
{
  mark_failure(meta, pk_predicate, retry_count);
  if (retry_count + 1 <= meta.max_retries_) {
    ai_column_defer(meta.table_id_, meta.column_id_,
                    ObTimeUtility::current_time());
  }
}

int build_pk_predicate(const ObIArray<ObString> &pk_columns, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  OZ(sql.append("concat("));
  for (int64_t i = 0; OB_SUCC(ret) && i < pk_columns.count(); ++i) {
    if (i > 0) {
      OZ(sql.append(", ' AND ', "));
    }
    OZ(sql.append_fmt("'`%.*s` = ', cast(t.`%.*s` as char)",
        pk_columns.at(i).length(), pk_columns.at(i).ptr(),
        pk_columns.at(i).length(), pk_columns.at(i).ptr()));
  }
  OZ(sql.append(")"));
  return ret;
}

int process_table(const ObString &database_name,
                  const ObString &table_name,
                  const ObIArray<ObString> &pk_columns,
                  const ObIArray<AIColumnMeta> &metas)
{
  int ret = OB_SUCCESS;
  const int64_t now = ObTimeUtility::current_time();
  ObSqlString sql;
  ObSqlString pk_pred_sql;
  ObArenaAllocator arena(ObModIds::OB_SQL_EXECUTOR);
  OZ(build_pk_predicate(pk_columns, pk_pred_sql));
  OZ(sql.append("select "));
  OZ(sql.append(pk_pred_sql.ptr()));
  OZ(sql.append(" as pk_pred"));
  for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
    OZ(sql.append_fmt(", `__ai_state_%.*s` as s%ld, "
        "cast(ifnull(`__ai_request_%.*s`, 0) as signed) as r%ld",
        metas.at(i).column_name_.length(), metas.at(i).column_name_.ptr(), i,
        metas.at(i).column_name_.length(), metas.at(i).column_name_.ptr(), i));
  }
  OZ(sql.append_fmt(" from `%.*s`.`%.*s` t where ",
      database_name.length(), database_name.ptr(),
      table_name.length(), table_name.ptr()));
  for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
    if (i > 0) {
      OZ(sql.append(" or "));
    }
    OZ(sql.append_fmt("`__ai_state_%.*s` is null or `__ai_state_%.*s` in (0, 3)",
        metas.at(i).column_name_.length(), metas.at(i).column_name_.ptr(),
        metas.at(i).column_name_.length(), metas.at(i).column_name_.ptr()));
  }
  OZ(sql.append(" limit 20"));

  HEAP_VAR(ObMySQLProxy::ReadResult, res) {
    common::sqlclient::ObMySQLResult *result = NULL;
    if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      LOG_WARN("ai column worker scan read failed", K(ret), "sql", sql.string());
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      int64_t scan_rows = 0;
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        ++scan_rows;
        ObString pk_predicate;
        if (OB_FAIL(result->get_varchar(static_cast<int64_t>(0), pk_predicate))) {
          break;
        }
        ObSEArray<AIColumnMeta, 4> pending_metas;
        ObSEArray<int64_t, 4> pending_retries;
        for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
          bool state_is_null = false;
          int64_t state = 0;
          int64_t retry_count = 0;
          ObObj state_obj;
          if (OB_FAIL(result->get_obj(static_cast<int64_t>(1 + 2 * i),
                                      state_obj, NULL))) {
          } else if (state_obj.is_null()) {
            state_is_null = true;
          } else {
            state = state_obj.get_int();
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(result->get_int(
                         static_cast<int64_t>(2 + 2 * i), retry_count))) {
          }
          if (OB_SUCC(ret) && (state_is_null || 0 == state || 3 == state)
              && !ai_column_deferred(metas.at(i).table_id_,
                                     metas.at(i).column_id_, now)) {
            OZ(pending_metas.push_back(metas.at(i)));
            OZ(pending_retries.push_back(retry_count));
          }
        }
        if (OB_FAIL(ret) || pending_metas.empty()) {
          continue;
        }
        ObSEArray<AIColumnMeta, 4> ok_metas;
        ObSEArray<ObString, 4> ok_results;
        ObSEArray<ObString, 4> ok_hashes;
        ObSEArray<ObString, 4> ok_prompts;
        for (int64_t i = 0; OB_SUCC(ret) && i < pending_metas.count(); ++i) {
          const AIColumnMeta &meta = pending_metas.at(i);
          bool is_embed = false;
          ObString model;
          ObString prompt_expr;
          ObString hint;
          ObString value;
          ObString hash;
          if (OB_FAIL(parse_ai_expression(arena, meta.expression_, is_embed,
                                          model, prompt_expr, hint))) {
            ret = OB_SUCCESS;
            handle_failure(meta, pk_predicate, pending_retries.at(i));
            continue;
          }
          ObSqlString call_sql;
          if (is_embed) {
            OZ(call_sql.append_fmt("select ai_embed('%.*s', (%.*s)) as r, "
                "md5(%.*s) as h from `%.*s`.`%.*s` where %.*s",
                model.length(), model.ptr(),
                prompt_expr.length(), prompt_expr.ptr(),
                prompt_expr.length(), prompt_expr.ptr(),
                database_name.length(), database_name.ptr(),
                table_name.length(), table_name.ptr(),
                pk_predicate.length(), pk_predicate.ptr()));
          } else if (!hint.empty()) {
            ObSqlString hint_literal;
            OZ(append_sql_literal(hint_literal, hint));
            OZ(call_sql.append_fmt("select ai_complete('%.*s', (%.*s), "
                "json_object('system', %.*s)) as r, md5(%.*s) as h "
                "from `%.*s`.`%.*s` where %.*s",
                model.length(), model.ptr(),
                prompt_expr.length(), prompt_expr.ptr(),
                hint_literal.length(), hint_literal.ptr(),
                prompt_expr.length(), prompt_expr.ptr(),
                database_name.length(), database_name.ptr(),
                table_name.length(), table_name.ptr(),
                pk_predicate.length(), pk_predicate.ptr()));
          } else {
            OZ(call_sql.append_fmt("select ai_complete('%.*s', (%.*s)) as r, "
                "md5(%.*s) as h from `%.*s`.`%.*s` where %.*s",
                model.length(), model.ptr(),
                prompt_expr.length(), prompt_expr.ptr(),
                prompt_expr.length(), prompt_expr.ptr(),
                database_name.length(), database_name.ptr(),
                table_name.length(), table_name.ptr(),
                pk_predicate.length(), pk_predicate.ptr()));
          }
          bool null_value = false;
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(read_two_strings(arena, call_sql.string(),
                                             value, hash, null_value))) {
            if (OB_ENTRY_NOT_EXIST == ret) {
              ret = OB_SUCCESS;
            } else {
              LOG_WARN("ai column model call failed", K(ret), K(meta.column_name_));
              ret = OB_SUCCESS;
              handle_failure(meta, pk_predicate, pending_retries.at(i));
            }
          } else if (null_value || value.empty()) {
            handle_failure(meta, pk_predicate, pending_retries.at(i));
          } else {
            OZ(ok_metas.push_back(meta));
            OZ(ok_results.push_back(value));
            OZ(ok_hashes.push_back(hash));
            OZ(ok_prompts.push_back(prompt_expr));
          }
        }
        if (OB_SUCC(ret) && !ok_metas.empty()) {
          if (OB_FAIL(publish_group(ok_metas, ok_results, ok_hashes,
                                    ok_prompts, pk_predicate))) {
            LOG_WARN("failed to publish ai column results", K(ret));
            for (int64_t i = 0; i < ok_metas.count(); ++i) {
              handle_failure(ok_metas.at(i), pk_predicate, 0);
            }
            ret = OB_SUCCESS;
          }
        }
      }

      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}
} // namespace

int ObAIColumnWorker::process_once()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else {
    ObArenaAllocator arena(ObModIds::OB_SQL_EXECUTOR);
    ObSEArray<AIColumnMeta, 16> metas;
    ObSqlString sql;
    OZ(sql.append(
        "select a.table_id, a.column_id, a.column_name, a.max_retries, a.normalized_expr, "
        "t.table_name, d.database_name "
        "from oceanbase.__all_ai_column a "
        "join oceanbase.__all_table t on t.table_id = a.table_id "
        "join oceanbase.__all_database d on d.database_id = t.database_id "
        "order by a.table_id, a.column_id"));
    HEAP_VAR(ObMySQLProxy::ReadResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          int64_t table_id = 0;
          int64_t column_id = 0;
          int64_t max_retries = 0;
          ObString column_name;
          ObString expression;
          ObString table_name;
          ObString database_name;
          AIColumnMeta meta;
          if (OB_FAIL(result->get_int("table_id", table_id))) {
          } else if (OB_FAIL(result->get_int("column_id", column_id))) {
          } else if (OB_FAIL(result->get_varchar("column_name", column_name))) {
          } else if (OB_FAIL(result->get_int("max_retries", max_retries))) {
          } else if (OB_FAIL(result->get_varchar("normalized_expr", expression))) {
          } else if (OB_FAIL(result->get_varchar("table_name", table_name))) {
          } else if (OB_FAIL(result->get_varchar("database_name", database_name))) {
          } else {
            meta.table_id_ = static_cast<uint64_t>(table_id);
            meta.column_id_ = column_id;
            meta.max_retries_ = max_retries;
            if (OB_FAIL(copy_string(arena, column_name, meta.column_name_))
                || OB_FAIL(copy_string(arena, expression, meta.expression_))
                || OB_FAIL(copy_string(arena, table_name, meta.table_name_))
                || OB_FAIL(copy_string(arena, database_name, meta.database_name_))) {
            } else {
              OZ(metas.push_back(meta));
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }

    // group by table and process
    for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ) {
      int64_t j = i;
      while (j < metas.count() && metas.at(j).table_id_ == metas.at(i).table_id_) {
        ++j;
      }
      // fetch pk columns for this table
      ObSqlString pk_sql;
      OZ(pk_sql.append_fmt(
          "select group_concat(column_name order by rowkey_position separator ',') "
          "as pk_cols from oceanbase.__all_column where table_id = %lu "
          "and rowkey_position > 0 and is_hidden = 0", metas.at(i).table_id_));
      // reuse expression field? no: query inside via helper below
      ObString pk_cols_str;
      ObString pk_cols_copy;
      {
        HEAP_VAR(ObMySQLProxy::ReadResult, pk_res) {
          common::sqlclient::ObMySQLResult *pk_result = NULL;
          if (OB_FAIL(GCTX.sql_proxy_->read(pk_res, pk_sql.ptr()))) {
          } else if (OB_ISNULL(pk_result = pk_res.get_result())) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(pk_result->next())) {
          } else if (OB_FAIL(pk_result->get_varchar("pk_cols", pk_cols_str))) {
          } else if (OB_FAIL(ob_write_string(arena, pk_cols_str, pk_cols_copy))) {
          }
        }
      }
      if (OB_SUCC(ret) && !pk_cols_copy.empty()) {
        pk_cols_str = pk_cols_copy;
        LOG_INFO("ai column worker table", "table", metas.at(i).table_name_,
                 "pk", pk_cols_str, "columns", j - i);
        ObSEArray<ObString, 8> pk_columns;
        int64_t begin = 0;
        while (OB_SUCC(ret) && begin <= pk_cols_str.length()) {
          int64_t end = begin;
          while (end < pk_cols_str.length() && ',' != pk_cols_str.ptr()[end]) ++end;
          ObString pk(static_cast<int32_t>(end - begin), pk_cols_str.ptr() + begin);
          if (!pk.empty()) {
            OZ(pk_columns.push_back(pk));
          }
          if (end >= pk_cols_str.length()) break;
          begin = end + 1;
        }
        ObSEArray<AIColumnMeta, 8> table_metas;
        for (int64_t k = i; k < j; ++k) {
          OZ(table_metas.push_back(metas.at(k)));
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(process_table(metas.at(i).database_name_, metas.at(i).table_name_,
                                    pk_columns, table_metas))) {
            LOG_WARN("failed to process ai column table", K(ret));            ret = OB_SUCCESS;
          }
        }
      }
      i = j;
    }
  }
  return ret;
}

void ObAIColumnWorker::run1()
{
  LOG_INFO("ai column worker started");
  while (!has_set_stop()) {
    int ret = OB_SUCCESS;
    if (OB_FAIL(process_once())) {
      LOG_WARN("ai column worker process failed", K(ret));
    }
    usleep(100 * 1000);
  }
  LOG_INFO("ai column worker stopped");
}

int ObAiService::start()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ai_column_worker_.set_thread_count(1))) {
  } else if (OB_FAIL(ai_column_worker_.start())) {
  }
  return ret;
}

void ObAiService::stop()
{
  ai_column_worker_.stop();
}

void ObAiService::wait()
{
  ai_column_worker_.wait();
}

ObAiService::ObAiService()
  : is_inited_(false)
{
}

ObAiService::~ObAiService()
{
  destroy();
}

int ObAiService::server_module_init(ObAiService* &ai_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ai_service)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_service->init())) {
  }
  return ret;
}

int ObAiService::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else {
    is_inited_ = true;
  }
  return ret;
}

void ObAiService::destroy()
{
  is_inited_ = false;
}

int ObAiService::get_ai_service_guard(ObAiServiceGuard &ai_service_guard)
{
  // TODO: implement after cache support
  int ret = OB_SUCCESS;
  return ret;
}

ObAiServiceGuard::ObAiServiceGuard()
  : local_allocator_(ObMemAttr("AiServiceGuard", ObCtxIds::DEFAULT_CTX_ID))
{
}

ObAiServiceGuard::~ObAiServiceGuard()
{
}

int ObAiServiceGuard::check_access_privilege()
{
  int ret = OB_SUCCESS;
  bool has_priv = false;

  sql::ObSQLSessionInfo *session = nullptr;
  if (OB_ISNULL(session = THIS_WORKER.get_session())) {
    // system background task thread do not have session, skip check
  } else {
    ObArenaAllocator tmp_allocator;
    share::schema::ObSchemaGetterGuard *schema_guard = session->get_cur_exec_ctx()->get_sql_ctx()->schema_guard_;
    if (OB_ISNULL(schema_guard)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sql::ObAIServiceEndpointPrivUtil priv_util(*schema_guard);
      share::schema::ObSessionPrivInfo session_priv;
      uint64_t user_id = session->get_priv_user_id();
      if (user_id == OB_INVALID_ID) {
        user_id = OB_SYS_USER_ID;
      }
      if (OB_FAIL(schema_guard->get_session_priv_info(
                                                    user_id,
                                                    session->get_database_name(),
                                                    session_priv))) {
      } else if (OB_FAIL(priv_util.check_access_ai_model_priv(tmp_allocator, session_priv, has_priv))) {
      } else if (!has_priv) {
        ret = OB_ERR_NO_PRIVILEGE;
        LOG_WARN("no privilege for ai model access", K(ret));
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "access ai model");
      }
    }
  }

  return ret;
}

int ObAiServiceGuard::get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (OB_FAIL(check_access_privilege())) {
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint(local_allocator_, name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

int ObAiServiceGuard::get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (need_check && OB_FAIL(check_access_privilege())) {
    LOG_WARN("failed to check access privilege", K(ret));
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(local_allocator_, ai_model_name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

} // namespace omt
} // namespace oceanbase
