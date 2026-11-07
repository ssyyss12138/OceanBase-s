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

#pragma once

// Generated from ob_inner_table_schema_def.py. Do not edit.
#include "share/storage/local_store.h"

namespace oceanbase { namespace share {

namespace deadlock_history
{
enum Column : uint32_t { EventId, DetectorId, ReportTime, CycleIdx, CycleSize, Role, PriorityLevel, Priority, CreateTime, StartDelay, Module, Visitor, Object, ExtraName1, ExtraValue1, ExtraName2, ExtraValue2, ExtraName3, ExtraValue3 };
inline const local::Schema &schema()
{
  static const auto schema = local_schema(2, {
    {local::Kind::Integer, false}, // event_id
    {local::Kind::Integer, false}, // detector_id
    {local::Kind::Integer, false}, // report_time
    {local::Kind::Integer, false}, // cycle_idx
    {local::Kind::Integer, false}, // cycle_size
    {local::Kind::Text, true}, // role
    {local::Kind::Text, true}, // priority_level
    {local::Kind::Integer, false}, // priority
    {local::Kind::Integer, false}, // create_time
    {local::Kind::Integer, false}, // start_delay
    {local::Kind::Text, true}, // module
    {local::Kind::Text, true}, // visitor
    {local::Kind::Text, true}, // object
    {local::Kind::Text, true}, // extra_name1
    {local::Kind::Text, true}, // extra_value1
    {local::Kind::Text, true}, // extra_name2
    {local::Kind::Text, true}, // extra_value2
    {local::Kind::Text, true}, // extra_name3
    {local::Kind::Text, true}, // extra_value3
  }, {EventId, DetectorId}, {ReportTime});
  return schema;
}
} // namespace deadlock_history

namespace tablet_meta
{
enum Column : uint32_t { GmtCreate, GmtModified, TabletId, CompactionScn, DataSize, RequiredSize, ReportScn, Status };
inline const local::Schema &schema()
{
  static const auto schema = local_schema(3, {
    {local::Kind::Integer, true}, // gmt_create
    {local::Kind::Integer, true}, // gmt_modified
    {local::Kind::Integer, false}, // tablet_id
    {local::Kind::Integer, false}, // compaction_scn
    {local::Kind::Integer, false}, // data_size
    {local::Kind::Integer, false}, // required_size
    {local::Kind::Integer, false}, // report_scn
    {local::Kind::Integer, false}, // status
  }, {TabletId}, {CompactionScn});
  return schema;
}
} // namespace tablet_meta

namespace tablet_checksum
{
enum Column : uint32_t { TabletId, CompactionScn, RowCount, DataChecksum, ColumnChecksums, BColumnChecksums, DataChecksumType };
inline const local::Schema &schema()
{
  static const auto schema = local_schema(4, {
    {local::Kind::Integer, false}, // tablet_id
    {local::Kind::Integer, false}, // compaction_scn
    {local::Kind::Integer, false}, // row_count
    {local::Kind::Integer, false}, // data_checksum
    {local::Kind::Text, true}, // column_checksums
    {local::Kind::Blob, true}, // b_column_checksums
    {local::Kind::Integer, false}, // data_checksum_type
  }, {TabletId}, {});
  return schema;
}
} // namespace tablet_checksum

namespace merge_info
{
enum Column : uint32_t { Id, FrozenScn, GlobalBroadcastScn, IsMergeError, LastMergedScn, MergeStatus, ErrorType, SuspendMerging, MergeStartTime, LastMergedTime };
inline const local::Schema &schema()
{
  static const auto schema = local_schema(0, {
    {local::Kind::Integer, false}, // id
    {local::Kind::Integer, false}, // frozen_scn
    {local::Kind::Integer, false}, // global_broadcast_scn
    {local::Kind::Integer, false}, // is_merge_error
    {local::Kind::Integer, false}, // last_merged_scn
    {local::Kind::Integer, false}, // merge_status
    {local::Kind::Integer, false}, // error_type
    {local::Kind::Integer, false}, // suspend_merging
    {local::Kind::Integer, false}, // merge_start_time
    {local::Kind::Integer, false}, // last_merged_time
  }, {Id}, {});
  return schema;
}
} // namespace merge_info

namespace checksum_error
{
enum Column : uint32_t { FrozenScn, IndexType, DataTableId, IndexTableId, DataTabletId, IndexTabletId, ColumnId, DataColumnChecksum, IndexColumnChecksum };
inline const local::Schema &schema()
{
  static const auto schema = local_schema(1, {
    {local::Kind::Integer, false}, // frozen_scn
    {local::Kind::Integer, false}, // index_type
    {local::Kind::Integer, false}, // data_table_id
    {local::Kind::Integer, false}, // index_table_id
    {local::Kind::Integer, false}, // data_tablet_id
    {local::Kind::Integer, false}, // index_tablet_id
    {local::Kind::Integer, false}, // column_id
    {local::Kind::Integer, false}, // data_column_checksum
    {local::Kind::Integer, false}, // index_column_checksum
  }, {FrozenScn, IndexType, DataTableId, IndexTableId, DataTabletId, IndexTabletId}, {});
  return schema;
}
} // namespace checksum_error

namespace admin_job
{
enum Column : uint32_t { JobId, GmtCreate, GmtModified, JobType, JobStatus, ResultCode };
inline const local::Schema &schema()
{
  static const auto schema = local_schema(5, {
    {local::Kind::Integer, false}, // job_id
    {local::Kind::Integer, true}, // gmt_create
    {local::Kind::Integer, true}, // gmt_modified
    {local::Kind::Text, false}, // job_type
    {local::Kind::Text, false}, // job_status
    {local::Kind::Integer, true}, // result_code
  }, {JobId}, {});
  return schema;
}
} // namespace admin_job

} } // namespace oceanbase::share
