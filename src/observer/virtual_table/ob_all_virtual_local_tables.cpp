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

#define USING_LOG_PREFIX SERVER

#include "observer/virtual_table/ob_all_virtual_local_tables.h"
#include "share/ob_server_struct.h"
#include "share/storage/local_store.h"
#include "lib/oblog/ob_log.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_print_utils.h"
#include "config_bridge.h"

#include <string_view>
#include "share/inner_table/local_table_schema.h"
namespace oceanbase
{
using namespace common;
using namespace share;

namespace observer
{

ObAllVirtualSysParameter::ObAllVirtualSysParameter()
  : is_inited_(false),
    cursor_(),
    row_idx_(0),
    name_(),
    gmt_create_(0),
    gmt_modified_(0),
    data_type_(),
    value_(),
    value_strict_(),
    info_(),
    need_reboot_(0),
    section_(),
    visible_level_(),
    config_version_(0),
    scope_(),
    source_(),
    edit_level_()
{
}

ObAllVirtualSysParameter::~ObAllVirtualSysParameter()
{
  reset();
}

void ObAllVirtualSysParameter::reset()
{
  cursor_.reset();
  is_inited_ = false;
  row_idx_ = 0;
  ObVirtualTableScannerIterator::reset();
}

int ObAllVirtualSysParameter::inner_open() { is_inited_ = true; row_idx_ = 0; return OB_SUCCESS; }

int ObAllVirtualSysParameter::inner_get_next_row(common::ObNewRow *&row)
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "not initialized", K(ret));
  } else {
    ret = get_next_row_from_local();
    if (OB_ITER_END == ret) {
      // End of result set
    } else if (OB_FAIL(ret)) {
      SERVER_LOG(WARN, "failed to get next row from local", K(ret));
    } else if (OB_FAIL(fill_cells())) {
      SERVER_LOG(WARN, "failed to fill cells", K(ret));
    } else {
      row = &cur_row_;
    }
  }

  return ret;
}

int ObAllVirtualSysParameter::get_next_row_from_local()
{
  if (row_idx_ >= config::parameter_count()) { return OB_ITER_END; }
  const auto parameter = config::parameter_row(row_idx_++);
  int ret = OB_SUCCESS;
  if (OB_SUCC(ret)) { ret = ob_write_string(*allocator_, ObString(parameter.name.size(), parameter.name.data()), name_); }
  gmt_create_ = 0;
  gmt_modified_ = 0;
  if (OB_SUCC(ret)) { ret = ob_write_string(*allocator_, ObString(parameter.data_type.size(), parameter.data_type.data()), data_type_); }
  if (OB_SUCC(ret)) { ret = ob_write_string(*allocator_, ObString(parameter.value.size(), parameter.value.data()), value_); }
  if (OB_SUCC(ret)) { ret = ob_write_string(*allocator_, ObString(parameter.value.size(), parameter.value.data()), value_strict_); }
  if (OB_SUCC(ret)) { ret = ob_write_string(*allocator_, ObString(parameter.info.size(), parameter.info.data()), info_); }
  need_reboot_ = (std::string_view(parameter.edit_level.data(), parameter.edit_level.size()) == "STATIC_EFFECTIVE");
  if (OB_SUCC(ret)) { ret = ob_write_string(*allocator_, ObString(parameter.section.size(), parameter.section.data()), section_); }
  visible_level_.reset();
  config_version_ = config::generation();
  if (OB_SUCC(ret)) { ret = ob_write_string(*allocator_, ObString(parameter.scope.size(), parameter.scope.data()), scope_); }
  if (OB_SUCC(ret)) { ret = ob_write_string(*allocator_, ObString(parameter.source.size(), parameter.source.data()), source_); }
  if (OB_SUCC(ret)) { ret = ob_write_string(*allocator_, ObString(parameter.edit_level.size(), parameter.edit_level.data()), edit_level_); }
  return ret;
}

int ObAllVirtualSysParameter::fill_cells()
{
  int ret = OB_SUCCESS;
  const int64_t col_count = output_column_ids_.count();
  ObObj *cells = cur_row_.cells_;

  if (OB_ISNULL(cells)) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "cells is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      uint64_t col_id = output_column_ids_.at(i);
      switch (col_id) {
        case OB_APP_MIN_COLUMN_ID + 0: {
          // name
          cells[i].set_varchar(name_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 1: {
          // gmt_create
          cells[i].set_int(gmt_create_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 2: {
          // gmt_modified
          cells[i].set_int(gmt_modified_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 3: {
          // data_type
          cells[i].set_varchar(data_type_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 4: {
          // value
          cells[i].set_varchar(value_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 5: {
          // value_strict
          cells[i].set_varchar(value_strict_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 6: {
          // info
          cells[i].set_varchar(info_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 7: {
          // need_reboot
          cells[i].set_int(need_reboot_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 8: {
          // section
          cells[i].set_varchar(section_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 9: {
          // visible_level
          cells[i].set_varchar(visible_level_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 10: {
          // config_version
          cells[i].set_uint64(config_version_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 11: {
          // scope
          cells[i].set_varchar(scope_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 12: {
          // source
          cells[i].set_varchar(source_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 13: {
          // edit_level
          cells[i].set_varchar(edit_level_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        default: {
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "invalid column id", K(ret), K(col_id));
          break;
        }
      }
    }
  }

  return ret;
}

ObAllVirtualDeadlockEventHistory::ObAllVirtualDeadlockEventHistory()
  : is_inited_(false),
    cursor_(),
    row_idx_(0),
    event_id_(0),
    detector_id_(0),
    report_time_(0),
    cycle_idx_(0),
    cycle_size_(0),
    role_(),
    priority_level_(),
    priority_(0),
    create_time_(0),
    start_delay_(0),
    module_(),
    visitor_(),
    object_(),
    extra_name1_(),
    extra_value1_(),
    extra_name2_(),
    extra_value2_(),
    extra_name3_(),
    extra_value3_()
{
}

ObAllVirtualDeadlockEventHistory::~ObAllVirtualDeadlockEventHistory()
{
  reset();
}

void ObAllVirtualDeadlockEventHistory::reset()
{
  cursor_.reset();
  is_inited_ = false;
  row_idx_ = 0;
  ObVirtualTableScannerIterator::reset();
}

int ObAllVirtualDeadlockEventHistory::inner_open()
{
  int ret = OB_SUCCESS;

  if (nullptr == GCTX.local_store_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local store is not initialized", K(ret));
  } else {
    const auto select_request = LocalRequest::select(deadlock_history::schema(), {deadlock_history::EventId, deadlock_history::DetectorId, deadlock_history::ReportTime, deadlock_history::CycleIdx, deadlock_history::CycleSize, deadlock_history::Role, deadlock_history::PriorityLevel, deadlock_history::Priority, deadlock_history::CreateTime, deadlock_history::StartDelay, deadlock_history::Module, deadlock_history::Visitor, deadlock_history::Object, deadlock_history::ExtraName1, deadlock_history::ExtraValue1, deadlock_history::ExtraName2, deadlock_history::ExtraValue2, deadlock_history::ExtraName3, deadlock_history::ExtraValue3});
    if (OB_FAIL(GCTX.local_store_->open_cursor(select_request, cursor_))) {
      SERVER_LOG(WARN, "failed to open local cursor", K(ret));
    } else {
      is_inited_ = true;
      row_idx_ = 0;
    }
  }

  return ret;
}

int ObAllVirtualDeadlockEventHistory::inner_get_next_row(common::ObNewRow *&row)
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "not initialized", K(ret));
  } else {
    ret = get_next_row_from_local();
    if (OB_ITER_END == ret) {
      // End of result set
    } else if (OB_FAIL(ret)) {
      SERVER_LOG(WARN, "failed to get next row from local", K(ret));
    } else if (OB_FAIL(fill_cells())) {
      SERVER_LOG(WARN, "failed to fill cells", K(ret));
    } else {
      row = &cur_row_;
    }
  }

  return ret;
}

int ObAllVirtualDeadlockEventHistory::get_next_row_from_local()
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local cursor is not open", K(ret));
  } else {
    LocalRowReader reader;
    ret = cursor_.next(reader);
    if (OB_SUCC(ret)) {
      event_id_ = reader.get_int64();
      detector_id_ = reader.get_int64();
      report_time_ = reader.get_int64();
      cycle_idx_ = reader.get_int64();
      cycle_size_ = reader.get_int64();
      int role_len = 0;
      const char *role_str = reader.get_text(&role_len);
      if (OB_ISNULL(role_str)) {
        role_.reset();
      } else {
        role_.assign_ptr(role_str, role_len);
      }
      int priority_level_len = 0;
      const char *priority_level_str = reader.get_text(&priority_level_len);
      if (OB_ISNULL(priority_level_str)) {
        priority_level_.reset();
      } else {
        priority_level_.assign_ptr(priority_level_str, priority_level_len);
      }
      priority_ = reader.get_int64();
      create_time_ = reader.get_int64();
      start_delay_ = reader.get_int64();
      int module_len = 0;
      const char *module_str = reader.get_text(&module_len);
      if (OB_ISNULL(module_str)) {
        module_.reset();
      } else {
        module_.assign_ptr(module_str, module_len);
      }
      int visitor_len = 0;
      const char *visitor_str = reader.get_text(&visitor_len);
      if (OB_ISNULL(visitor_str)) {
        visitor_.reset();
      } else {
        visitor_.assign_ptr(visitor_str, visitor_len);
      }
      int object_len = 0;
      const char *object_str = reader.get_text(&object_len);
      if (OB_ISNULL(object_str)) {
        object_.reset();
      } else {
        object_.assign_ptr(object_str, object_len);
      }
      int extra_name1_len = 0;
      const char *extra_name1_str = reader.get_text(&extra_name1_len);
      if (OB_ISNULL(extra_name1_str)) {
        extra_name1_.reset();
      } else {
        extra_name1_.assign_ptr(extra_name1_str, extra_name1_len);
      }
      int extra_value1_len = 0;
      const char *extra_value1_str = reader.get_text(&extra_value1_len);
      if (OB_ISNULL(extra_value1_str)) {
        extra_value1_.reset();
      } else {
        extra_value1_.assign_ptr(extra_value1_str, extra_value1_len);
      }
      int extra_name2_len = 0;
      const char *extra_name2_str = reader.get_text(&extra_name2_len);
      if (OB_ISNULL(extra_name2_str)) {
        extra_name2_.reset();
      } else {
        extra_name2_.assign_ptr(extra_name2_str, extra_name2_len);
      }
      int extra_value2_len = 0;
      const char *extra_value2_str = reader.get_text(&extra_value2_len);
      if (OB_ISNULL(extra_value2_str)) {
        extra_value2_.reset();
      } else {
        extra_value2_.assign_ptr(extra_value2_str, extra_value2_len);
      }
      int extra_name3_len = 0;
      const char *extra_name3_str = reader.get_text(&extra_name3_len);
      if (OB_ISNULL(extra_name3_str)) {
        extra_name3_.reset();
      } else {
        extra_name3_.assign_ptr(extra_name3_str, extra_name3_len);
      }
      int extra_value3_len = 0;
      const char *extra_value3_str = reader.get_text(&extra_value3_len);
      if (OB_ISNULL(extra_value3_str)) {
        extra_value3_.reset();
      } else {
        extra_value3_.assign_ptr(extra_value3_str, extra_value3_len);
      }
    } else if (OB_ITER_END == ret) {
      // End of result set
    } else {
      SERVER_LOG(WARN, "failed to step query", K(ret));
    }
  }

  return ret;
}

int ObAllVirtualDeadlockEventHistory::fill_cells()
{
  int ret = OB_SUCCESS;
  const int64_t col_count = output_column_ids_.count();
  ObObj *cells = cur_row_.cells_;

  if (OB_ISNULL(cells)) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "cells is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      uint64_t col_id = output_column_ids_.at(i);
      switch (col_id) {
        case OB_APP_MIN_COLUMN_ID + 0: {
          // event_id
          cells[i].set_int(event_id_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 1: {
          // detector_id
          cells[i].set_int(detector_id_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 2: {
          // report_time
          cells[i].set_int(report_time_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 3: {
          // cycle_idx
          cells[i].set_int(cycle_idx_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 4: {
          // cycle_size
          cells[i].set_int(cycle_size_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 5: {
          // role
          cells[i].set_varchar(role_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 6: {
          // priority_level
          cells[i].set_varchar(priority_level_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 7: {
          // priority
          cells[i].set_int(priority_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 8: {
          // create_time
          cells[i].set_int(create_time_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 9: {
          // start_delay
          cells[i].set_int(start_delay_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 10: {
          // module
          cells[i].set_varchar(module_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 11: {
          // visitor
          cells[i].set_varchar(visitor_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 12: {
          // object
          cells[i].set_varchar(object_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 13: {
          // extra_name1
          cells[i].set_varchar(extra_name1_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 14: {
          // extra_value1
          cells[i].set_varchar(extra_value1_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 15: {
          // extra_name2
          cells[i].set_varchar(extra_name2_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 16: {
          // extra_value2
          cells[i].set_varchar(extra_value2_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 17: {
          // extra_name3
          cells[i].set_varchar(extra_name3_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 18: {
          // extra_value3
          cells[i].set_varchar(extra_value3_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        default: {
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "invalid column id", K(ret), K(col_id));
          break;
        }
      }
    }
  }

  return ret;
}

ObAllVirtualTabletMetaTable::ObAllVirtualTabletMetaTable()
  : is_inited_(false),
    cursor_(),
    row_idx_(0),
    tablet_id_(0),
    gmt_create_(0),
    gmt_modified_(0),
    compaction_scn_(0),
    data_size_(0),
    required_size_(0),
    report_scn_(0),
    status_(0)
{
}

ObAllVirtualTabletMetaTable::~ObAllVirtualTabletMetaTable()
{
  reset();
}

void ObAllVirtualTabletMetaTable::reset()
{
  cursor_.reset();
  is_inited_ = false;
  row_idx_ = 0;
  ObVirtualTableScannerIterator::reset();
}

int ObAllVirtualTabletMetaTable::inner_open()
{
  int ret = OB_SUCCESS;

  if (nullptr == GCTX.local_store_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local store is not initialized", K(ret));
  } else {
    const auto select_request = LocalRequest::select(tablet_meta::schema(), {tablet_meta::TabletId, tablet_meta::GmtCreate, tablet_meta::GmtModified, tablet_meta::CompactionScn, tablet_meta::DataSize, tablet_meta::RequiredSize, tablet_meta::ReportScn, tablet_meta::Status});
    if (OB_FAIL(GCTX.local_store_->open_cursor(select_request, cursor_))) {
      SERVER_LOG(WARN, "failed to open local cursor", K(ret));
    } else {
      is_inited_ = true;
      row_idx_ = 0;
    }
  }

  return ret;
}

int ObAllVirtualTabletMetaTable::inner_get_next_row(common::ObNewRow *&row)
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "not initialized", K(ret));
  } else {
    ret = get_next_row_from_local();
    if (OB_ITER_END == ret) {
      // End of result set
    } else if (OB_FAIL(ret)) {
      SERVER_LOG(WARN, "failed to get next row from local", K(ret));
    } else if (OB_FAIL(fill_cells())) {
      SERVER_LOG(WARN, "failed to fill cells", K(ret));
    } else {
      row = &cur_row_;
    }
  }

  return ret;
}

int ObAllVirtualTabletMetaTable::get_next_row_from_local()
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local cursor is not open", K(ret));
  } else {
    LocalRowReader reader;
    ret = cursor_.next(reader);
    if (OB_SUCC(ret)) {
      tablet_id_ = reader.get_int64();
      gmt_create_ = reader.get_int64();
      gmt_modified_ = reader.get_int64();
      compaction_scn_ = reader.get_int64();
      data_size_ = reader.get_int64();
      required_size_ = reader.get_int64();
      report_scn_ = reader.get_int64();
      status_ = reader.get_int64();
    } else if (OB_ITER_END == ret) {
      // End of result set
    } else {
      SERVER_LOG(WARN, "failed to step query", K(ret));
    }
  }

  return ret;
}

int ObAllVirtualTabletMetaTable::fill_cells()
{
  int ret = OB_SUCCESS;
  const int64_t col_count = output_column_ids_.count();
  ObObj *cells = cur_row_.cells_;

  if (OB_ISNULL(cells)) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "cells is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      uint64_t col_id = output_column_ids_.at(i);
      switch (col_id) {
        case OB_APP_MIN_COLUMN_ID + 0: {
          // tablet_id
          cells[i].set_int(tablet_id_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 1: {
          // gmt_create
          cells[i].set_int(gmt_create_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 2: {
          // gmt_modified
          cells[i].set_int(gmt_modified_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 3: {
          // compaction_scn
          cells[i].set_uint64(compaction_scn_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 4: {
          // data_size
          cells[i].set_int(data_size_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 5: {
          // required_size
          cells[i].set_int(required_size_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 6: {
          // report_scn
          cells[i].set_uint64(report_scn_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 7: {
          // status
          cells[i].set_int(status_);
          break;
        }
        default: {
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "invalid column id", K(ret), K(col_id));
          break;
        }
      }
    }
  }

  return ret;
}

ObAllVirtualTabletLocalChecksum::ObAllVirtualTabletLocalChecksum()
  : is_inited_(false),
    cursor_(),
    row_idx_(0),
    tablet_id_(0),
    compaction_scn_(0),
    row_count_(0),
    data_checksum_(0),
    column_checksums_(),
    b_column_checksums_(),
    data_checksum_type_(0)
{
}

ObAllVirtualTabletLocalChecksum::~ObAllVirtualTabletLocalChecksum()
{
  reset();
}

void ObAllVirtualTabletLocalChecksum::reset()
{
  cursor_.reset();
  is_inited_ = false;
  row_idx_ = 0;
  ObVirtualTableScannerIterator::reset();
}

int ObAllVirtualTabletLocalChecksum::inner_open()
{
  int ret = OB_SUCCESS;

  if (nullptr == GCTX.local_store_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local store is not initialized", K(ret));
  } else {
    const auto select_request = LocalRequest::select(tablet_checksum::schema(), {tablet_checksum::TabletId, tablet_checksum::CompactionScn, tablet_checksum::RowCount, tablet_checksum::DataChecksum, tablet_checksum::ColumnChecksums, tablet_checksum::BColumnChecksums, tablet_checksum::DataChecksumType});
    if (OB_FAIL(GCTX.local_store_->open_cursor(select_request, cursor_))) {
      SERVER_LOG(WARN, "failed to open local cursor", K(ret));
    } else {
      is_inited_ = true;
      row_idx_ = 0;
    }
  }

  return ret;
}

int ObAllVirtualTabletLocalChecksum::inner_get_next_row(common::ObNewRow *&row)
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "not initialized", K(ret));
  } else {
    ret = get_next_row_from_local();
    if (OB_ITER_END == ret) {
      // End of result set
    } else if (OB_FAIL(ret)) {
      SERVER_LOG(WARN, "failed to get next row from local", K(ret));
    } else if (OB_FAIL(fill_cells())) {
      SERVER_LOG(WARN, "failed to fill cells", K(ret));
    } else {
      row = &cur_row_;
    }
  }

  return ret;
}

int ObAllVirtualTabletLocalChecksum::get_next_row_from_local()
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local cursor is not open", K(ret));
  } else {
    LocalRowReader reader;
    ret = cursor_.next(reader);
    if (OB_SUCC(ret)) {
      tablet_id_ = reader.get_int64();
      compaction_scn_ = reader.get_int64();
      row_count_ = reader.get_int64();
      data_checksum_ = reader.get_int64();
      int column_checksums_len = 0;
      const char *column_checksums_str = reader.get_text(&column_checksums_len);
      if (OB_ISNULL(column_checksums_str)) {
        column_checksums_.reset();
      } else {
        column_checksums_.assign_ptr(column_checksums_str, column_checksums_len);
      }
      int b_column_checksums_len = 0;
      const void *b_column_checksums_ptr = reader.get_blob(&b_column_checksums_len);
      if (OB_ISNULL(b_column_checksums_ptr)) {
        b_column_checksums_.reset();
      } else {
        b_column_checksums_.assign_ptr(static_cast<const char *>(b_column_checksums_ptr), b_column_checksums_len);
      }
      data_checksum_type_ = reader.get_int64();
    } else if (OB_ITER_END == ret) {
      // End of result set
    } else {
      SERVER_LOG(WARN, "failed to step query", K(ret));
    }
  }

  return ret;
}

int ObAllVirtualTabletLocalChecksum::fill_cells()
{
  int ret = OB_SUCCESS;
  const int64_t col_count = output_column_ids_.count();
  ObObj *cells = cur_row_.cells_;

  if (OB_ISNULL(cells)) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "cells is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      uint64_t col_id = output_column_ids_.at(i);
      switch (col_id) {
        case OB_APP_MIN_COLUMN_ID + 0: {
          // tablet_id
          cells[i].set_int(tablet_id_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 1: {
          // compaction_scn
          cells[i].set_uint64(compaction_scn_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 2: {
          // row_count
          cells[i].set_int(row_count_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 3: {
          // data_checksum
          cells[i].set_int(data_checksum_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 4: {
          // column_checksums
          cells[i].set_varchar(column_checksums_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 5: {
          // b_column_checksums
          cells[i].set_varbinary(b_column_checksums_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 6: {
          // data_checksum_type
          cells[i].set_int(data_checksum_type_);
          break;
        }
        default: {
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "invalid column id", K(ret), K(col_id));
          break;
        }
      }
    }
  }

  return ret;
}

ObAllVirtualMergeInfo::ObAllVirtualMergeInfo()
  : is_inited_(false),
    cursor_(),
    row_idx_(0),
    id_(0),
    frozen_scn_(0),
    global_broadcast_scn_(0),
    is_merge_error_(0),
    last_merged_scn_(0),
    merge_status_(0),
    error_type_(0),
    suspend_merging_(0),
    merge_start_time_(0),
    last_merged_time_(0)
{
}

ObAllVirtualMergeInfo::~ObAllVirtualMergeInfo()
{
  reset();
}

void ObAllVirtualMergeInfo::reset()
{
  cursor_.reset();
  is_inited_ = false;
  row_idx_ = 0;
  ObVirtualTableScannerIterator::reset();
}

int ObAllVirtualMergeInfo::inner_open()
{
  int ret = OB_SUCCESS;

  if (nullptr == GCTX.local_store_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local store is not initialized", K(ret));
  } else {
    const auto select_request = LocalRequest::select(merge_info::schema(), {merge_info::Id, merge_info::FrozenScn, merge_info::GlobalBroadcastScn, merge_info::IsMergeError, merge_info::LastMergedScn, merge_info::MergeStatus, merge_info::ErrorType, merge_info::SuspendMerging, merge_info::MergeStartTime, merge_info::LastMergedTime});
    if (OB_FAIL(GCTX.local_store_->open_cursor(select_request, cursor_))) {
      SERVER_LOG(WARN, "failed to open local cursor", K(ret));
    } else {
      is_inited_ = true;
      row_idx_ = 0;
    }
  }

  return ret;
}

int ObAllVirtualMergeInfo::inner_get_next_row(common::ObNewRow *&row)
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "not initialized", K(ret));
  } else {
    ret = get_next_row_from_local();
    if (OB_ITER_END == ret) {
      // End of result set
    } else if (OB_FAIL(ret)) {
      SERVER_LOG(WARN, "failed to get next row from local", K(ret));
    } else if (OB_FAIL(fill_cells())) {
      SERVER_LOG(WARN, "failed to fill cells", K(ret));
    } else {
      row = &cur_row_;
    }
  }

  return ret;
}

int ObAllVirtualMergeInfo::get_next_row_from_local()
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local cursor is not open", K(ret));
  } else {
    LocalRowReader reader;
    ret = cursor_.next(reader);
    if (OB_SUCC(ret)) {
      id_ = reader.get_int64();
      frozen_scn_ = reader.get_int64();
      global_broadcast_scn_ = reader.get_int64();
      is_merge_error_ = reader.get_int64();
      last_merged_scn_ = reader.get_int64();
      merge_status_ = reader.get_int64();
      error_type_ = reader.get_int64();
      suspend_merging_ = reader.get_int64();
      merge_start_time_ = reader.get_int64();
      last_merged_time_ = reader.get_int64();
    } else if (OB_ITER_END == ret) {
      // End of result set
    } else {
      SERVER_LOG(WARN, "failed to step query", K(ret));
    }
  }

  return ret;
}

int ObAllVirtualMergeInfo::fill_cells()
{
  int ret = OB_SUCCESS;
  const int64_t col_count = output_column_ids_.count();
  ObObj *cells = cur_row_.cells_;

  if (OB_ISNULL(cells)) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "cells is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      uint64_t col_id = output_column_ids_.at(i);
      switch (col_id) {
        case OB_APP_MIN_COLUMN_ID + 0: {
          // id
          cells[i].set_int(id_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 1: {
          // frozen_scn
          cells[i].set_uint64(frozen_scn_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 2: {
          // global_broadcast_scn
          cells[i].set_uint64(global_broadcast_scn_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 3: {
          // is_merge_error
          cells[i].set_int(is_merge_error_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 4: {
          // last_merged_scn
          cells[i].set_uint64(last_merged_scn_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 5: {
          // merge_status
          cells[i].set_int(merge_status_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 6: {
          // error_type
          cells[i].set_int(error_type_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 7: {
          // suspend_merging
          cells[i].set_int(suspend_merging_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 8: {
          // merge_start_time
          cells[i].set_int(merge_start_time_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 9: {
          // last_merged_time
          cells[i].set_int(last_merged_time_);
          break;
        }
        default: {
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "invalid column id", K(ret), K(col_id));
          break;
        }
      }
    }
  }

  return ret;
}

ObAllVirtualColumnChecksumErrorInfo::ObAllVirtualColumnChecksumErrorInfo()
  : is_inited_(false),
    cursor_(),
    row_idx_(0),
    frozen_scn_(0),
    index_type_(0),
    data_table_id_(0),
    index_table_id_(0),
    data_tablet_id_(0),
    index_tablet_id_(0),
    column_id_(0),
    data_column_checksum_(0),
    index_column_checksum_(0)
{
}

ObAllVirtualColumnChecksumErrorInfo::~ObAllVirtualColumnChecksumErrorInfo()
{
  reset();
}

void ObAllVirtualColumnChecksumErrorInfo::reset()
{
  cursor_.reset();
  is_inited_ = false;
  row_idx_ = 0;
  ObVirtualTableScannerIterator::reset();
}

int ObAllVirtualColumnChecksumErrorInfo::inner_open()
{
  int ret = OB_SUCCESS;

  if (nullptr == GCTX.local_store_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local store is not initialized", K(ret));
  } else {
    const auto select_request = LocalRequest::select(checksum_error::schema(), {checksum_error::FrozenScn, checksum_error::IndexType, checksum_error::DataTableId, checksum_error::IndexTableId, checksum_error::DataTabletId, checksum_error::IndexTabletId, checksum_error::ColumnId, checksum_error::DataColumnChecksum, checksum_error::IndexColumnChecksum});
    if (OB_FAIL(GCTX.local_store_->open_cursor(select_request, cursor_))) {
      SERVER_LOG(WARN, "failed to open local cursor", K(ret));
    } else {
      is_inited_ = true;
      row_idx_ = 0;
    }
  }

  return ret;
}

int ObAllVirtualColumnChecksumErrorInfo::inner_get_next_row(common::ObNewRow *&row)
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "not initialized", K(ret));
  } else {
    ret = get_next_row_from_local();
    if (OB_ITER_END == ret) {
      // End of result set
    } else if (OB_FAIL(ret)) {
      SERVER_LOG(WARN, "failed to get next row from local", K(ret));
    } else if (OB_FAIL(fill_cells())) {
      SERVER_LOG(WARN, "failed to fill cells", K(ret));
    } else {
      row = &cur_row_;
    }
  }

  return ret;
}

int ObAllVirtualColumnChecksumErrorInfo::get_next_row_from_local()
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local cursor is not open", K(ret));
  } else {
    LocalRowReader reader;
    ret = cursor_.next(reader);
    if (OB_SUCC(ret)) {
      frozen_scn_ = reader.get_int64();
      index_type_ = reader.get_int64();
      data_table_id_ = reader.get_int64();
      index_table_id_ = reader.get_int64();
      data_tablet_id_ = reader.get_int64();
      index_tablet_id_ = reader.get_int64();
      column_id_ = reader.get_int64();
      data_column_checksum_ = reader.get_int64();
      index_column_checksum_ = reader.get_int64();
    } else if (OB_ITER_END == ret) {
      // End of result set
    } else {
      SERVER_LOG(WARN, "failed to step query", K(ret));
    }
  }

  return ret;
}

int ObAllVirtualColumnChecksumErrorInfo::fill_cells()
{
  int ret = OB_SUCCESS;
  const int64_t col_count = output_column_ids_.count();
  ObObj *cells = cur_row_.cells_;

  if (OB_ISNULL(cells)) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "cells is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      uint64_t col_id = output_column_ids_.at(i);
      switch (col_id) {
        case OB_APP_MIN_COLUMN_ID + 0: {
          // frozen_scn
          cells[i].set_uint64(frozen_scn_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 1: {
          // index_type
          cells[i].set_int(index_type_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 2: {
          // data_table_id
          cells[i].set_int(data_table_id_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 3: {
          // index_table_id
          cells[i].set_int(index_table_id_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 4: {
          // data_tablet_id
          cells[i].set_int(data_tablet_id_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 5: {
          // index_tablet_id
          cells[i].set_int(index_tablet_id_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 6: {
          // column_id
          cells[i].set_int(column_id_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 7: {
          // data_column_checksum
          cells[i].set_int(data_column_checksum_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 8: {
          // index_column_checksum
          cells[i].set_int(index_column_checksum_);
          break;
        }
        default: {
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "invalid column id", K(ret), K(col_id));
          break;
        }
      }
    }
  }

  return ret;
}

ObAllVirtualRootserviceJob::ObAllVirtualRootserviceJob()
  : is_inited_(false),
    cursor_(),
    row_idx_(0),
    job_id_(0),
    gmt_create_(0),
    gmt_modified_(0),
    job_type_(),
    job_status_(),
    result_code_(0)
{
}

ObAllVirtualRootserviceJob::~ObAllVirtualRootserviceJob()
{
  reset();
}

void ObAllVirtualRootserviceJob::reset()
{
  cursor_.reset();
  is_inited_ = false;
  row_idx_ = 0;
  ObVirtualTableScannerIterator::reset();
}

int ObAllVirtualRootserviceJob::inner_open()
{
  int ret = OB_SUCCESS;

  if (nullptr == GCTX.local_store_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local store is not initialized", K(ret));
  } else {
    const auto select_request = LocalRequest::select(admin_job::schema(), {admin_job::JobId, admin_job::GmtCreate, admin_job::GmtModified, admin_job::JobType, admin_job::JobStatus, admin_job::ResultCode});
    if (OB_FAIL(GCTX.local_store_->open_cursor(select_request, cursor_))) {
      SERVER_LOG(WARN, "failed to open local cursor", K(ret));
    } else {
      is_inited_ = true;
      row_idx_ = 0;
    }
  }

  return ret;
}

int ObAllVirtualRootserviceJob::inner_get_next_row(common::ObNewRow *&row)
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "not initialized", K(ret));
  } else {
    ret = get_next_row_from_local();
    if (OB_ITER_END == ret) {
      // End of result set
    } else if (OB_FAIL(ret)) {
      SERVER_LOG(WARN, "failed to get next row from local", K(ret));
    } else if (OB_FAIL(fill_cells())) {
      SERVER_LOG(WARN, "failed to fill cells", K(ret));
    } else {
      row = &cur_row_;
    }
  }

  return ret;
}

int ObAllVirtualRootserviceJob::get_next_row_from_local()
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "local cursor is not open", K(ret));
  } else {
    LocalRowReader reader;
    ret = cursor_.next(reader);
    if (OB_SUCC(ret)) {
      job_id_ = reader.get_int64();
      gmt_create_ = reader.get_int64();
      gmt_modified_ = reader.get_int64();
      int job_type_len = 0;
      const char *job_type_str = reader.get_text(&job_type_len);
      if (OB_ISNULL(job_type_str)) {
        job_type_.reset();
      } else {
        job_type_.assign_ptr(job_type_str, job_type_len);
      }
      int job_status_len = 0;
      const char *job_status_str = reader.get_text(&job_status_len);
      if (OB_ISNULL(job_status_str)) {
        job_status_.reset();
      } else {
        job_status_.assign_ptr(job_status_str, job_status_len);
      }
      result_code_ = reader.get_int64();
    } else if (OB_ITER_END == ret) {
      // End of result set
    } else {
      SERVER_LOG(WARN, "failed to step query", K(ret));
    }
  }

  return ret;
}

int ObAllVirtualRootserviceJob::fill_cells()
{
  int ret = OB_SUCCESS;
  const int64_t col_count = output_column_ids_.count();
  ObObj *cells = cur_row_.cells_;

  if (OB_ISNULL(cells)) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "cells is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      uint64_t col_id = output_column_ids_.at(i);
      switch (col_id) {
        case OB_APP_MIN_COLUMN_ID + 0: {
          // job_id
          cells[i].set_int(job_id_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 1: {
          // gmt_create
          cells[i].set_int(gmt_create_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 2: {
          // gmt_modified
          cells[i].set_int(gmt_modified_);
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 3: {
          // job_type
          cells[i].set_varchar(job_type_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 4: {
          // job_status
          cells[i].set_varchar(job_status_);
          cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 5: {
          // result_code
          cells[i].set_int(result_code_);
          break;
        }
        default: {
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "invalid column id", K(ret), K(col_id));
          break;
        }
      }
    }
  }

  return ret;
}

} // namespace observer
} // namespace oceanbase
