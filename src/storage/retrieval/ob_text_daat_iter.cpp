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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (nullptr != param.base_param_->relevance_expr_
      && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    use_batch_union_ = nullptr == param.base_param_->relevance_expr_
        && sql::NATURAL_LANGUAGE_MODE == mode_flag_ && !function_lookup_mode_
        && nullptr == param.base_param_->dim_weights_ && 1.0 == param.base_param_->field_boost_
        && param.base_param_->eval_ctx_->is_vectorized()
        && common::ObVarcharType == param.base_param_->id_proj_expr_->datum_meta_.type_
        && common::CS_TYPE_BINARY == param.base_param_->id_proj_expr_->datum_meta_.cs_type_
        && dim_iters_->count() > 0 && dim_iters_->count() <= MAX_BATCH_UNION_DIMS;
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  use_batch_union_ = false;
  for (auto &cursor : union_cursors_) {
    cursor = UnionCursor{};
  }
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  for (auto &cursor : union_cursors_) {
    cursor = UnionCursor{};
  }
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  if (!use_batch_union_ || dim_iters_->count() == 0 || dim_iters_->count() > MAX_BATCH_UNION_DIMS) {
    return ObSRDaaTIterImpl::get_next_rows(capacity, count);
  }
  int ret = OB_SUCCESS;
  count = 0;
  if (IS_NOT_INIT || capacity < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (iter_param_->limit_param_->is_valid()
      && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    ret = OB_ITER_END;
  } else {
    const int64_t dim_count = dim_iters_->count();
    auto *const *iters = &dim_iters_->at(0);
    const int64_t output_capacity = MIN(capacity, iter_param_->max_batch_size_);
    while (OB_SUCC(ret) && count < output_capacity) {
      int64_t minimum = -1;
      uint64_t equal_dims = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
        UnionCursor &cursor = union_cursors_[i];
        if (!cursor.ended_ && cursor.pos_ == cursor.size_) {
          auto *iter = static_cast<ObTextRetrievalDaaTTokenIter *>(iters[i]);
          if (OB_FAIL(iter->get_next_doc_id_batch(cursor.ids_, cursor.size_))) {
            if (OB_ITER_END == ret) {
              cursor.ended_ = true;
              ret = OB_SUCCESS;
            }
          }
          cursor.pos_ = 0;
        }
        if (OB_SUCC(ret) && !cursor.ended_) {
          // Only binary document IDs enter this path. Their stored bytes have
          // the same order as the original null-safe binary comparator.
          const int cmp = minimum < 0 ? -1 : cursor.ids_[cursor.pos_].get_datum().get_string().compare(
              union_cursors_[minimum].ids_[union_cursors_[minimum].pos_].get_datum().get_string());
          if (cmp < 0) {
            minimum = i;
            equal_dims = uint64_t{1} << i;
          } else if (0 == cmp) {
            equal_dims |= uint64_t{1} << i;
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (minimum < 0) {
        ret = OB_ITER_END;
      } else {
        double relevance = 0.0;
        bool valid = false;
        // Retain the collector so minimum_should_match and repeated query
        // terms keep their existing semantics. No numerical score is exposed.
        for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
          if ((equal_dims & (uint64_t{1} << i)) != 0
              && OB_FAIL(relevance_collector_->collect_one_dim(i, 1.0))) {
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(relevance_collector_->get_result(relevance, valid))) {
        } else if (OB_SUCC(ret) && valid && OB_FAIL(filter_on_demand(count, relevance, valid))) {
        } else if (OB_SUCC(ret) && valid) {
          const UnionCursor &cursor = union_cursors_[minimum];
          // Copy to the owned output before any cursor can refill its buffer.
          ret = cache_result(count, cursor.ids_[cursor.pos_].get_datum(), relevance);
        }
        for (int64_t i = 0; i < dim_count; ++i) {
          if ((equal_dims & (uint64_t{1} << i)) != 0) {
            ++union_cursors_[i].pos_;
          }
        }
      }
    }
  }
  if ((OB_SUCC(ret) || OB_ITER_END == ret) && count > 0) {
    const int project_ret = project_results(count);
    if (OB_SUCCESS != project_ret) {
      ret = project_ret;
    }
  }
  return ret;
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (nullptr != iter_param_->relevance_expr_) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
