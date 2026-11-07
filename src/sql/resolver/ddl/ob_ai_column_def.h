/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */
#ifndef OCEANBASE_SQL_RESOLVER_DDL_OB_AI_COLUMN_DEF_H_
#define OCEANBASE_SQL_RESOLVER_DDL_OB_AI_COLUMN_DEF_H_

#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace sql
{

struct ObAIColumnDef
{
  ObAIColumnDef()
      : max_retries_(0),
        definition_version_(1),
        column_id_(OB_INVALID_ID),
        state_column_id_(OB_INVALID_ID),
        request_column_id_(OB_INVALID_ID),
        result_spec_column_id_(OB_INVALID_ID),
        result_type_(0),
        vector_dimension_(0)
  {}

  common::ObString column_name_;
  common::ObString normalized_expr_;
  common::ObString dependency_column_ids_;
  int64_t max_retries_;
  int64_t definition_version_;
  uint64_t column_id_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  int64_t result_type_;
  int64_t vector_dimension_;

  TO_STRING_KV(K_(column_name), K_(normalized_expr), K_(dependency_column_ids),
               K_(max_retries), K_(definition_version), K_(column_id),
               K_(state_column_id), K_(request_column_id),
               K_(result_spec_column_id), K_(result_type),
               K_(vector_dimension));
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_DDL_OB_AI_COLUMN_DEF_H_
