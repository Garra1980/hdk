/*
 * Copyright 2020 OmniSci, Inc.
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

#include "DdlCommandExecutor.h"

#include <boost/algorithm/string/predicate.hpp>

#include "rapidjson/document.h"

// Note: avoid adding #include(s) that require thrift

#include "Catalog/Catalog.h"
#include "Catalog/SysCatalog.h"
#include "LockMgr/LockMgr.h"
#include "Parser/ParserNode.h"
#include "Shared/StringTransform.h"

#include "QueryEngine/Execute.h"  // Executor::getArenaBlockSize()
#include "QueryEngine/ExternalCacheInvalidators.h"
#include "QueryEngine/ResultSetBuilder.h"

extern bool g_enable_fsi;

bool DdlCommand::isDefaultServer(const std::string& server_name) {
  return boost::iequals(server_name.substr(0, 7), "omnisci");
}

namespace {
template <class LockType>
std::tuple<const TableDescriptor*,
           std::unique_ptr<lockmgr::TableSchemaLockContainer<LockType>>>
get_table_descriptor_with_lock(const Catalog_Namespace::Catalog& cat,
                               const std::string& table_name,
                               const bool populate_fragmenter) {
  const TableDescriptor* td{nullptr};
  std::unique_ptr<lockmgr::TableSchemaLockContainer<LockType>> td_with_lock =
      std::make_unique<lockmgr::TableSchemaLockContainer<LockType>>(
          lockmgr::TableSchemaLockContainer<LockType>::acquireTableDescriptor(
              cat, table_name, populate_fragmenter));
  CHECK(td_with_lock);
  td = (*td_with_lock)();
  CHECK(td);
  return std::make_tuple(td, std::move(td_with_lock));
}

struct AggregratedStorageStats : public File_Namespace::StorageStats {
  int32_t min_epoch;
  int32_t max_epoch;
  int32_t min_epoch_floor;
  int32_t max_epoch_floor;

  AggregratedStorageStats(const File_Namespace::StorageStats& storage_stats)
      : File_Namespace::StorageStats(storage_stats)
      , min_epoch(storage_stats.epoch)
      , max_epoch(storage_stats.epoch)
      , min_epoch_floor(storage_stats.epoch_floor)
      , max_epoch_floor(storage_stats.epoch_floor) {}

  void aggregate(const File_Namespace::StorageStats& storage_stats) {
    metadata_file_count += storage_stats.metadata_file_count;
    total_metadata_file_size += storage_stats.total_metadata_file_size;
    total_metadata_page_count += storage_stats.total_metadata_page_count;
    if (storage_stats.total_free_metadata_page_count) {
      if (total_free_metadata_page_count) {
        total_free_metadata_page_count.value() +=
            storage_stats.total_free_metadata_page_count.value();
      } else {
        total_free_metadata_page_count = storage_stats.total_free_metadata_page_count;
      }
    }
    data_file_count += storage_stats.data_file_count;
    total_data_file_size += storage_stats.total_data_file_size;
    total_data_page_count += storage_stats.total_data_page_count;
    if (storage_stats.total_free_data_page_count) {
      if (total_free_data_page_count) {
        total_free_data_page_count.value() +=
            storage_stats.total_free_data_page_count.value();
      } else {
        total_free_data_page_count = storage_stats.total_free_data_page_count;
      }
    }
    min_epoch = std::min(min_epoch, storage_stats.epoch);
    max_epoch = std::max(max_epoch, storage_stats.epoch);
    min_epoch_floor = std::min(min_epoch_floor, storage_stats.epoch_floor);
    max_epoch_floor = std::max(max_epoch_floor, storage_stats.epoch_floor);
  }
};

AggregratedStorageStats get_agg_storage_stats(const TableDescriptor* td,
                                              const Catalog_Namespace::Catalog* catalog) {
  const auto global_file_mgr = catalog->getDataMgr().getGlobalFileMgr();
  std::optional<AggregratedStorageStats> agg_storage_stats;
  agg_storage_stats =
      global_file_mgr->getStorageStats(catalog->getDatabaseId(), td->tableId);
  CHECK(agg_storage_stats.has_value());
  return agg_storage_stats.value();
}

std::unique_ptr<RexLiteral> genLiteralStr(std::string val) {
  return std::unique_ptr<RexLiteral>(
      new RexLiteral(val, SQLTypes::kTEXT, SQLTypes::kTEXT, 0, 0, 0, 0));
}

std::unique_ptr<RexLiteral> genLiteralTimestamp(time_t val) {
  return std::unique_ptr<RexLiteral>(new RexLiteral(
      (int64_t)val, SQLTypes::kTIMESTAMP, SQLTypes::kTIMESTAMP, 0, 8, 0, 8));
}

std::unique_ptr<RexLiteral> genLiteralBigInt(int64_t val) {
  return std::unique_ptr<RexLiteral>(
      new RexLiteral(val, SQLTypes::kBIGINT, SQLTypes::kBIGINT, 0, 8, 0, 8));
}

std::unique_ptr<RexLiteral> genLiteralBoolean(bool val) {
  return std::unique_ptr<RexLiteral>(
      // new RexLiteral(val, SQLTypes::kBOOLEAN, SQLTypes::kBOOLEAN, 0, 0, 0, 0));
      new RexLiteral(
          (int64_t)(val ? 1 : 0), SQLTypes::kBIGINT, SQLTypes::kBIGINT, 0, 8, 0, 8));
}

void set_headers_with_type(
    std::vector<TargetMetaInfo>& label_infos,
    const std::vector<std::tuple<std::string, SQLTypes, bool>>& headers) {
  for (const auto& header : headers) {
    auto [_val, _type, _notnull] = header;
    if (_type == kBIGINT || _type == kTEXT || _type == kTIMESTAMP || _type == kBOOLEAN) {
      label_infos.emplace_back(_val, SQLTypeInfo(_type, _notnull));
    } else {
      UNREACHABLE() << "Unsupported type provided for header. SQL type: "
                    << to_string(_type);
    }
  }
}

void add_table_details(std::vector<RelLogicalValues::RowValues>& logical_values,
                       const TableDescriptor* logical_table,
                       const AggregratedStorageStats& agg_storage_stats) {
  logical_values.emplace_back(RelLogicalValues::RowValues{});
  logical_values.back().emplace_back(genLiteralBigInt(logical_table->tableId));
  logical_values.back().emplace_back(genLiteralStr(logical_table->tableName));
  logical_values.back().emplace_back(genLiteralBigInt(logical_table->nColumns));
  logical_values.back().emplace_back(genLiteralBoolean(false)); // sharded
  logical_values.back().emplace_back(genLiteralBigInt(0)); // nShards
  logical_values.back().emplace_back(genLiteralBigInt(logical_table->maxRows));
  logical_values.back().emplace_back(genLiteralBigInt(logical_table->maxFragRows));
  logical_values.back().emplace_back(genLiteralBigInt(logical_table->maxRollbackEpochs));
  logical_values.back().emplace_back(genLiteralBigInt(agg_storage_stats.min_epoch));
  logical_values.back().emplace_back(genLiteralBigInt(agg_storage_stats.max_epoch));
  logical_values.back().emplace_back(genLiteralBigInt(agg_storage_stats.min_epoch_floor));
  logical_values.back().emplace_back(genLiteralBigInt(agg_storage_stats.max_epoch_floor));
  logical_values.back().emplace_back(
      genLiteralBigInt(agg_storage_stats.metadata_file_count));
  logical_values.back().emplace_back(
      genLiteralBigInt(agg_storage_stats.total_metadata_file_size));
  logical_values.back().emplace_back(
      genLiteralBigInt(agg_storage_stats.total_metadata_page_count));

  if (agg_storage_stats.total_free_metadata_page_count) {
    logical_values.back().emplace_back(
        genLiteralBigInt(agg_storage_stats.total_free_metadata_page_count.value()));
  } else {
    logical_values.back().emplace_back(genLiteralBigInt(NULL_BIGINT));
  }

  logical_values.back().emplace_back(genLiteralBigInt(agg_storage_stats.data_file_count));
  logical_values.back().emplace_back(
      genLiteralBigInt(agg_storage_stats.total_data_file_size));
  logical_values.back().emplace_back(
      genLiteralBigInt(agg_storage_stats.total_data_page_count));

  if (agg_storage_stats.total_free_data_page_count) {
    logical_values.back().emplace_back(
        genLiteralBigInt(agg_storage_stats.total_free_data_page_count.value()));
  } else {
    logical_values.back().emplace_back(genLiteralBigInt(NULL_BIGINT));
  }
}

// -----------------------------------------------------------------------
// class: JsonColumnSqlType
//   Defined & Implemented here to avoid exposing rapidjson in the header file
// -----------------------------------------------------------------------

/// declare this class scoped local to avoid exposing rapidjson in the header file
class JsonColumnSqlType : public ddl_utils::SqlType {
 public:
  JsonColumnSqlType(const rapidjson::Value& data_type)
      : ddl_utils::SqlType(getSqlType(data_type),
                           getParam1(data_type),
                           getParam2(data_type),
                           isArray(data_type),
                           getArraySize(data_type)) {}

 private:
  static SQLTypes getSqlType(const rapidjson::Value& data_type);
  static SQLTypes getSqlType(const std::string& type);
  static int getParam1(const rapidjson::Value& data_type);
  static int getParam2(const rapidjson::Value& data_type);
  static bool isArray(const rapidjson::Value& data_type);
  static int getArraySize(const rapidjson::Value& data_type);
};

class JsonColumnEncoding : public ddl_utils::Encoding {
 public:
  JsonColumnEncoding(const rapidjson::Value& data_type)
      : ddl_utils::Encoding(getEncodingName(data_type), getEncodingParam(data_type)) {}

 private:
  static std::string* getEncodingName(const rapidjson::Value& data_type);
  static int getEncodingParam(const rapidjson::Value& data_type);
};

// -----------------------------------------------------------------------
// class DdlCommandDataImpl:
//
// Concrete class to cache parse data
//   Defined & Implemented here to avoid exposing rapidjson in the header file
//   Helper/access fns available to get useful pieces of cache data
// -----------------------------------------------------------------------
class DdlCommandDataImpl : public DdlCommandData {
 public:
  DdlCommandDataImpl(const std::string& ddl_statement);
  ~DdlCommandDataImpl() override;

  // The full query available for futher analysis
  const rapidjson::Value& query() const;

  // payload as extracted from the query
  const rapidjson::Value& payload() const;

  // commandStr extracted from the payload
  std::string commandStr() override;

  rapidjson::Document ddl_query;
};

DdlCommandDataImpl::DdlCommandDataImpl(const std::string& ddl_statement)
    : DdlCommandData(ddl_statement) {
  ddl_query.Parse(ddl_statement);
}

DdlCommandDataImpl::~DdlCommandDataImpl() {}

const rapidjson::Value& DdlCommandDataImpl::query() const {
  return ddl_query;
}

const rapidjson::Value& DdlCommandDataImpl::payload() const {
  CHECK(ddl_query.HasMember("payload"));
  CHECK(ddl_query["payload"].IsObject());
  return ddl_query["payload"];
}

std::string DdlCommandDataImpl::commandStr() {
  if (ddl_query.IsObject() && ddl_query.HasMember("payload") &&
      ddl_query["payload"].IsObject()) {
    auto& payload = ddl_query["payload"];
    if (payload.HasMember("command") && payload["command"].IsString()) {
      return payload["command"].GetString();
    }
  }
  return "";
}

// Helper Fn to get the payload from the abstract base class
const rapidjson::Value& extractPayload(const DdlCommandData& ddl_data) {
  const DdlCommandDataImpl* data = static_cast<const DdlCommandDataImpl*>(&ddl_data);
  return data->payload();
}

const rapidjson::Value* extractFilters(const rapidjson::Value& payload) {
  const rapidjson::Value* filters = nullptr;
  if (payload.HasMember("filters") && payload["filters"].IsArray()) {
    filters = &payload["filters"];
  }
  return filters;
}

}  // namespace

DdlCommandExecutor::DdlCommandExecutor(
    const std::string& ddl_statement,
    std::shared_ptr<Catalog_Namespace::SessionInfo const> session_ptr)
    : session_ptr_(session_ptr) {
  CHECK(!ddl_statement.empty());
  ddl_statement_ = ddl_statement;

  // parse the incoming query,
  //    cache the parsed rapidjson object inside a DdlCommandDataImpl
  //    store the "abstract/base class" reference in ddl_data_
  DdlCommandDataImpl* ddl_query_data = new DdlCommandDataImpl(ddl_statement);
  ddl_data_ = std::unique_ptr<DdlCommandData>(ddl_query_data);

  VLOG(2) << "Parsing JSON DDL from Calcite: " << ddl_statement;
  auto& ddl_query = ddl_query_data->query();
  CHECK(ddl_query.IsObject()) << ddl_statement;
  CHECK(ddl_query.HasMember("payload"));
  CHECK(ddl_query["payload"].IsObject());
  const auto& payload = ddl_query["payload"].GetObject();
  CHECK(payload.HasMember("command"));
  CHECK(payload["command"].IsString());
  ddl_command_ = payload["command"].GetString();
}

ExecutionResult DdlCommandExecutor::execute() {
  ExecutionResult result;

  // the following commands use parser node locking to ensure safe concurrent access
  if (ddl_command_ == "CREATE_TABLE") {
    auto create_table_stmt = Parser::CreateTableStmt(extractPayload(*ddl_data_));
    create_table_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "CREATE_VIEW") {
    auto create_view_stmt = Parser::CreateViewStmt(extractPayload(*ddl_data_));
    create_view_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "DROP_TABLE") {
    auto drop_table_stmt = Parser::DropTableStmt(extractPayload(*ddl_data_));
    drop_table_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "DROP_VIEW") {
    auto drop_view_stmt = Parser::DropViewStmt(extractPayload(*ddl_data_));
    drop_view_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "RENAME_TABLE") {
    auto rename_table_stmt = Parser::RenameTableStmt(extractPayload(*ddl_data_));
    rename_table_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "ALTER_TABLE") {
    auto stmt = Parser::AlterTableStmt::delegate(extractPayload(*ddl_data_));
    if (stmt != nullptr) {
      stmt->execute(*session_ptr_);
    }
    return result;
  } else if (ddl_command_ == "TRUNCATE_TABLE") {
    auto truncate_table_stmt = Parser::TruncateTableStmt(extractPayload(*ddl_data_));
    truncate_table_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "DUMP_TABLE") {
    auto dump_table_stmt = Parser::DumpTableStmt(extractPayload(*ddl_data_));
    dump_table_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "RESTORE_TABLE") {
    auto restore_table_stmt = Parser::RestoreTableStmt(extractPayload(*ddl_data_));
    restore_table_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "OPTIMIZE_TABLE") {
    auto optimize_table_stmt = Parser::OptimizeTableStmt(extractPayload(*ddl_data_));
    optimize_table_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "SHOW_CREATE_TABLE") {
    auto show_create_table_stmt = Parser::ShowCreateTableStmt(extractPayload(*ddl_data_));
    show_create_table_stmt.execute(*session_ptr_);
    const auto create_string = show_create_table_stmt.getCreateStmt();
    result.updateResultSet(create_string, ExecutionResult::SimpleResult);
    return result;
  } else if (ddl_command_ == "COPY_TABLE") {
    auto copy_table_stmt = Parser::CopyTableStmt(extractPayload(*ddl_data_));
    copy_table_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "EXPORT_QUERY") {
    auto export_query_stmt = Parser::ExportQueryStmt(extractPayload(*ddl_data_));
    export_query_stmt.execute(*session_ptr_);
    return result;

  } else if (ddl_command_ == "CREATE_DB") {
    auto create_db_stmt = Parser::CreateDBStmt(extractPayload(*ddl_data_));
    create_db_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "DROP_DB") {
    auto drop_db_stmt = Parser::DropDBStmt(extractPayload(*ddl_data_));
    drop_db_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "RENAME_DB") {
    auto rename_db_stmt = Parser::RenameDBStmt(extractPayload(*ddl_data_));
    rename_db_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "CREATE_USER") {
    auto create_user_stmt = Parser::CreateUserStmt(extractPayload(*ddl_data_));
    create_user_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "DROP_USER") {
    auto drop_user_stmt = Parser::DropUserStmt(extractPayload(*ddl_data_));
    drop_user_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "ALTER_USER") {
    auto alter_user_stmt = Parser::AlterUserStmt(extractPayload(*ddl_data_));
    alter_user_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "RENAME_USER") {
    auto rename_user_stmt = Parser::RenameUserStmt(extractPayload(*ddl_data_));
    rename_user_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "CREATE_ROLE") {
    auto create_role_stmt = Parser::CreateRoleStmt(extractPayload(*ddl_data_));
    create_role_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "DROP_ROLE") {
    auto drop_role_stmt = Parser::DropRoleStmt(extractPayload(*ddl_data_));
    drop_role_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "GRANT_ROLE") {
    auto grant_role_stmt = Parser::GrantRoleStmt(extractPayload(*ddl_data_));
    grant_role_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "REVOKE_ROLE") {
    auto revoke_role_stmt = Parser::RevokeRoleStmt(extractPayload(*ddl_data_));
    revoke_role_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "GRANT_PRIVILEGE") {
    auto grant_privilege_stmt = Parser::GrantPrivilegesStmt(extractPayload(*ddl_data_));
    grant_privilege_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "REVOKE_PRIVILEGE") {
    auto revoke_privileges_stmt =
        Parser::RevokePrivilegesStmt(extractPayload(*ddl_data_));
    revoke_privileges_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "CREATE_DATAFRAME") {
    auto create_dataframe_stmt = Parser::CreateDataframeStmt(extractPayload(*ddl_data_));
    create_dataframe_stmt.execute(*session_ptr_);
    return result;
  } else if (ddl_command_ == "VALIDATE_SYSTEM") {
    // VALIDATE should have been excuted in outer context before it reaches here
    UNREACHABLE();
  }

  // the following commands require a global unique lock until proper table locking has
  // been implemented and/or verified
  auto execute_write_lock = mapd_unique_lock<mapd_shared_mutex>(
      *legacylockmgr::LockMgr<mapd_shared_mutex, bool>::getMutex(
          legacylockmgr::ExecutorOuterLock, true));
  // TODO(vancouver): add appropriate table locking

  if (ddl_command_ == "SHOW_TABLES") {
    result = ShowTablesCommand{*ddl_data_, session_ptr_}.execute();
  } else if (ddl_command_ == "SHOW_TABLE_DETAILS") {
    result = ShowTableDetailsCommand{*ddl_data_, session_ptr_}.execute();
  } else if (ddl_command_ == "SHOW_DATABASES") {
    result = ShowDatabasesCommand{*ddl_data_, session_ptr_}.execute();
  } else if (ddl_command_ == "SHOW_DISK_CACHE_USAGE") {
    result = ShowDiskCacheUsageCommand{*ddl_data_, session_ptr_}.execute();
  } else if (ddl_command_ == "SHOW_USER_DETAILS") {
    result = ShowUserDetailsCommand{*ddl_data_, session_ptr_}.execute();
  } else if (ddl_command_ == "REASSIGN_OWNED") {
    result = ReassignOwnedCommand{*ddl_data_, session_ptr_}.execute();
  } else {
    throw std::runtime_error("Unsupported DDL command " + ddl_command_);
  }

  return result;
}

bool DdlCommandExecutor::isShowUserSessions() {
  return (ddl_command_ == "SHOW_USER_SESSIONS");
}

bool DdlCommandExecutor::isShowQueries() {
  return (ddl_command_ == "SHOW_QUERIES");
}

bool DdlCommandExecutor::isKillQuery() {
  return (ddl_command_ == "KILL_QUERY");
}

bool DdlCommandExecutor::isShowCreateTable() {
  return (ddl_command_ == "SHOW_CREATE_TABLE");
}

bool DdlCommandExecutor::isAlterSystemClear() {
  return (ddl_command_ == "ALTER_SYSTEM_CLEAR");
}

std::string DdlCommandExecutor::returnCacheType() {
  CHECK(ddl_command_ == "ALTER_SYSTEM_CLEAR");
  auto& ddl_payload = extractPayload(*ddl_data_);
  CHECK(ddl_payload.HasMember("cacheType"));
  CHECK(ddl_payload["cacheType"].IsString());
  return ddl_payload["cacheType"].GetString();
}

DistributedExecutionDetails DdlCommandExecutor::getDistributedExecutionDetails() {
  DistributedExecutionDetails execution_details;
  if (ddl_command_ == "CREATE_DATAFRAME" || ddl_command_ == "RENAME_TABLE" ||
      ddl_command_ == "ALTER_TABLE" || ddl_command_ == "CREATE_TABLE" ||
      ddl_command_ == "DROP_TABLE" || ddl_command_ == "TRUNCATE_TABLE" ||
      ddl_command_ == "DUMP_TABLE" || ddl_command_ == "RESTORE_TABLE" ||
      ddl_command_ == "OPTIMIZE_TABLE" || ddl_command_ == "CREATE_VIEW" ||
      ddl_command_ == "DROP_VIEW" || ddl_command_ == "CREATE_DB" ||
      ddl_command_ == "DROP_DB" || ddl_command_ == "RENAME_DB" ||
      ddl_command_ == "CREATE_USER" || ddl_command_ == "DROP_USER" ||
      ddl_command_ == "ALTER_USER" || ddl_command_ == "RENAME_USER" ||
      ddl_command_ == "CREATE_ROLE" || ddl_command_ == "DROP_ROLE" ||
      ddl_command_ == "GRANT_ROLE" || ddl_command_ == "REVOKE_ROLE" ||
      ddl_command_ == "REASSIGN_OWNED") {
    // group user/role/db commands
    execution_details.execution_location = ExecutionLocation::ALL_NODES;
    execution_details.aggregation_type = AggregationType::NONE;
  } else if (ddl_command_ == "GRANT_PRIVILEGE" || ddl_command_ == "REVOKE_PRIVILEGE") {
    auto& ddl_payload = extractPayload(*ddl_data_);
    CHECK(ddl_payload.HasMember("type"));
    const std::string& targetType = ddl_payload["type"].GetString();
    if (targetType == "DASHBOARD") {
      // dashboard commands should run on Aggregator alone
      execution_details.execution_location = ExecutionLocation::AGGREGATOR_ONLY;
      execution_details.aggregation_type = AggregationType::NONE;
    } else {
      execution_details.execution_location = ExecutionLocation::ALL_NODES;
      execution_details.aggregation_type = AggregationType::NONE;
    }

  } else if (ddl_command_ == "SHOW_TABLE_DETAILS") {
    execution_details.execution_location = ExecutionLocation::LEAVES_ONLY;
    execution_details.aggregation_type = AggregationType::UNION;
  } else {
    // Commands that fall here : COPY_TABLE, EXPORT_QUERY, etc.
    execution_details.execution_location = ExecutionLocation::AGGREGATOR_ONLY;
    execution_details.aggregation_type = AggregationType::NONE;
  }
  return execution_details;
}

const std::string DdlCommandExecutor::getTargetQuerySessionToKill() {
  // caller should check whether DDL indicates KillQuery request
  // i.e., use isKillQuery() before calling this function
  auto& ddl_payload = extractPayload(*ddl_data_);
  CHECK(isKillQuery());
  CHECK(ddl_payload.HasMember("querySession"));
  const std::string& query_session = ddl_payload["querySession"].GetString();
  // regex matcher for public_session: start_time{3}-session_id{4} (Example:819-4RDo)
  boost::regex session_id_regex{R"([0-9]{3}-[a-zA-Z0-9]{4})",
                                boost::regex::extended | boost::regex::icase};
  if (!boost::regex_match(query_session, session_id_regex)) {
    throw std::runtime_error(
        "Please provide the correct session ID of the query that you want to interrupt.");
  }
  return query_session;
}

const std::string DdlCommandExecutor::commandStr() {
  return ddl_command_;
}

SQLTypes JsonColumnSqlType::getSqlType(const rapidjson::Value& data_type) {
  CHECK(data_type.IsObject());
  CHECK(data_type.HasMember("type"));
  CHECK(data_type["type"].IsString());

  std::string type = data_type["type"].GetString();
  if (boost::iequals(type, "ARRAY")) {
    CHECK(data_type.HasMember("array"));
    CHECK(data_type["array"].IsObject());

    const auto& array = data_type["array"].GetObject();
    CHECK(array.HasMember("elementType"));
    CHECK(array["elementType"].IsString());
    type = array["elementType"].GetString();
  }
  return getSqlType(type);
}

SQLTypes JsonColumnSqlType::getSqlType(const std::string& type) {
  if (boost::iequals(type, "BIGINT")) {
    return kBIGINT;
  }
  if (boost::iequals(type, "BOOLEAN")) {
    return kBOOLEAN;
  }
  if (boost::iequals(type, "DATE")) {
    return kDATE;
  }
  if (boost::iequals(type, "DECIMAL")) {
    return kDECIMAL;
  }
  if (boost::iequals(type, "DOUBLE")) {
    return kDOUBLE;
  }
  if (boost::iequals(type, "FLOAT")) {
    return kFLOAT;
  }
  if (boost::iequals(type, "INTEGER")) {
    return kINT;
  }
  if (boost::iequals(type, "LINESTRING")) {
    return kLINESTRING;
  }
  if (boost::iequals(type, "MULTIPOLYGON")) {
    return kMULTIPOLYGON;
  }
  if (boost::iequals(type, "POINT")) {
    return kPOINT;
  }
  if (boost::iequals(type, "POLYGON")) {
    return kPOLYGON;
  }
  if (boost::iequals(type, "SMALLINT")) {
    return kSMALLINT;
  }
  if (boost::iequals(type, "TEXT")) {
    return kTEXT;
  }
  if (boost::iequals(type, "TIME")) {
    return kTIME;
  }
  if (boost::iequals(type, "TIMESTAMP")) {
    return kTIMESTAMP;
  }
  if (boost::iequals(type, "TINYINT")) {
    return kTINYINT;
  }

  throw std::runtime_error{"Unsupported type \"" + type + "\" specified."};
}

int JsonColumnSqlType::getParam1(const rapidjson::Value& data_type) {
  int param1 = -1;
  CHECK(data_type.IsObject());
  if (data_type.HasMember("precision") && !data_type["precision"].IsNull()) {
    CHECK(data_type["precision"].IsInt());
    param1 = data_type["precision"].GetInt();
  } else if (auto type = getSqlType(data_type); IS_GEO(type)) {
    param1 = static_cast<int>(kGEOMETRY);
  }
  return param1;
}

int JsonColumnSqlType::getParam2(const rapidjson::Value& data_type) {
  int param2 = 0;
  CHECK(data_type.IsObject());
  if (data_type.HasMember("scale") && !data_type["scale"].IsNull()) {
    CHECK(data_type["scale"].IsInt());
    param2 = data_type["scale"].GetInt();
  } else if (auto type = getSqlType(data_type); IS_GEO(type) &&
                                                data_type.HasMember("coordinateSystem") &&
                                                !data_type["coordinateSystem"].IsNull()) {
    CHECK(data_type["coordinateSystem"].IsInt());
    param2 = data_type["coordinateSystem"].GetInt();
  }
  return param2;
}

bool JsonColumnSqlType::isArray(const rapidjson::Value& data_type) {
  CHECK(data_type.IsObject());
  CHECK(data_type.HasMember("type"));
  CHECK(data_type["type"].IsString());
  return boost::iequals(data_type["type"].GetString(), "ARRAY");
}

int JsonColumnSqlType::getArraySize(const rapidjson::Value& data_type) {
  int size = -1;
  if (isArray(data_type)) {
    CHECK(data_type.HasMember("array"));
    CHECK(data_type["array"].IsObject());

    const auto& array = data_type["array"].GetObject();
    if (array.HasMember("size") && !array["size"].IsNull()) {
      CHECK(array["size"].IsInt());
      size = array["size"].GetInt();
    }
  }
  return size;
}

std::string* JsonColumnEncoding::getEncodingName(const rapidjson::Value& data_type) {
  CHECK(data_type.IsObject());
  CHECK(data_type.HasMember("encoding"));
  CHECK(data_type["encoding"].IsObject());

  const auto& encoding = data_type["encoding"].GetObject();
  CHECK(encoding.HasMember("type"));
  CHECK(encoding["type"].IsString());
  return new std::string(encoding["type"].GetString());
}

int JsonColumnEncoding::getEncodingParam(const rapidjson::Value& data_type) {
  CHECK(data_type.IsObject());
  CHECK(data_type.HasMember("encoding"));
  CHECK(data_type["encoding"].IsObject());

  int encoding_size = 0;
  const auto& encoding = data_type["encoding"].GetObject();
  if (encoding.HasMember("size") && !encoding["size"].IsNull()) {
    CHECK(encoding["size"].IsInt());
    encoding_size = encoding["size"].GetInt();
  }
  return encoding_size;
}

ShowTablesCommand::ShowTablesCommand(
    const DdlCommandData& ddl_data,
    std::shared_ptr<Catalog_Namespace::SessionInfo const> session_ptr)
    : DdlCommand(ddl_data, session_ptr) {}

ExecutionResult ShowTablesCommand::execute() {
  // Get all table names in the same way as OmniSql \t command

  // label_infos -> column labels
  std::vector<std::string> labels{"table_name"};
  std::vector<TargetMetaInfo> label_infos;
  for (const auto& label : labels) {
    label_infos.emplace_back(label, SQLTypeInfo(kTEXT, true));
  }

  // Get all table names
  auto cat_ptr = session_ptr_->get_catalog_ptr();
  auto cur_user = session_ptr_->get_currentUser();
  auto table_names = cat_ptr->getTableNamesForUser(cur_user, GET_PHYSICAL_TABLES);

  // logical_values -> table data
  std::vector<RelLogicalValues::RowValues> logical_values;
  for (auto table_name : table_names) {
    logical_values.emplace_back(RelLogicalValues::RowValues{});
    logical_values.back().emplace_back(genLiteralStr(table_name));
  }

  // Create ResultSet
  std::shared_ptr<ResultSet> rSet = std::shared_ptr<ResultSet>(
      ResultSetLogicalValuesBuilder::create(label_infos, logical_values));

  return ExecutionResult(rSet, label_infos);
}

ShowTableDetailsCommand::ShowTableDetailsCommand(
    const DdlCommandData& ddl_data,
    std::shared_ptr<Catalog_Namespace::SessionInfo const> session_ptr)
    : DdlCommand(ddl_data, session_ptr) {
  auto& ddl_payload = extractPayload(ddl_data_);
  if (ddl_payload.HasMember("tableNames")) {
    CHECK(ddl_payload["tableNames"].IsArray());
    for (const auto& table_name : ddl_payload["tableNames"].GetArray()) {
      CHECK(table_name.IsString());
    }
  }
}

ExecutionResult ShowTableDetailsCommand::execute() {
  const auto catalog = session_ptr_->get_catalog_ptr();
  std::vector<std::string> filtered_table_names = getFilteredTableNames();

  std::vector<TargetMetaInfo> label_infos;
  set_headers_with_type(label_infos,
                        {// { label, type, notNull }
                         {"table_id", kBIGINT, true},
                         {"table_name", kTEXT, true},
                         {"column_count", kBIGINT, true},
                         {"is_sharded_table", kBOOLEAN, true},
                         {"shard_count", kBIGINT, true},
                         {"max_rows", kBIGINT, true},
                         {"fragment_size", kBIGINT, true},
                         {"max_rollback_epochs", kBIGINT, true},
                         {"min_epoch", kBIGINT, true},
                         {"max_epoch", kBIGINT, true},
                         {"min_epoch_floor", kBIGINT, true},
                         {"max_epoch_floor", kBIGINT, true},
                         {"metadata_file_count", kBIGINT, true},
                         {"total_metadata_file_size", kBIGINT, true},
                         {"total_metadata_page_count", kBIGINT, true},
                         {"total_free_metadata_page_count", kBIGINT, false},
                         {"data_file_count", kBIGINT, true},
                         {"total_data_file_size", kBIGINT, true},
                         {"total_data_page_count", kBIGINT, true},
                         {"total_free_data_page_count", kBIGINT, false}});

  std::vector<RelLogicalValues::RowValues> logical_values;
  for (const auto& table_name : filtered_table_names) {
    auto [td, td_with_lock] =
        get_table_descriptor_with_lock<lockmgr::ReadLock>(*catalog, table_name, false);
    auto agg_storage_stats = get_agg_storage_stats(td, catalog.get());
    add_table_details(logical_values, td, agg_storage_stats);
  }

  // Create ResultSet
  std::shared_ptr<ResultSet> rSet = std::shared_ptr<ResultSet>(
      ResultSetLogicalValuesBuilder::create(label_infos, logical_values));

  return ExecutionResult(rSet, label_infos);
}

std::vector<std::string> ShowTableDetailsCommand::getFilteredTableNames() {
  const auto catalog = session_ptr_->get_catalog_ptr();
  auto& ddl_payload = extractPayload(ddl_data_);
  auto all_table_names =
      catalog->getTableNamesForUser(session_ptr_->get_currentUser(), GET_PHYSICAL_TABLES);
  std::transform(all_table_names.begin(),
                 all_table_names.end(),
                 all_table_names.begin(),
                 [](const std::string& s) { return to_upper(s); });
  std::vector<std::string> filtered_table_names;
  if (ddl_payload.HasMember("tableNames")) {
    std::set<std::string> all_table_names_set(all_table_names.begin(),
                                              all_table_names.end());
    for (const auto& table_name_json : ddl_payload["tableNames"].GetArray()) {
      std::string table_name = table_name_json.GetString();
      if (all_table_names_set.find(to_upper(table_name)) == all_table_names_set.end()) {
        throw std::runtime_error{"Unable to show table details for table: " + table_name +
                                 ". Table does not exist."};
      }
      auto [td, td_with_lock] =
          get_table_descriptor_with_lock<lockmgr::ReadLock>(*catalog, table_name, false);
      if (td->isTemporaryTable()) {
        throw std::runtime_error{
            "SHOW TABLE DETAILS is not supported for temporary tables. Table name: " +
            table_name + "."};
      }
      filtered_table_names.emplace_back(table_name);
    }
  } else {
    for (const auto& table_name : all_table_names) {
      auto [td, td_with_lock] =
          get_table_descriptor_with_lock<lockmgr::ReadLock>(*catalog, table_name, false);
      if (td->isTemporaryTable()) {
        continue;
      }
      filtered_table_names.emplace_back(table_name);
    }
  }
  return filtered_table_names;
}

ShowDatabasesCommand::ShowDatabasesCommand(
    const DdlCommandData& ddl_data,
    std::shared_ptr<Catalog_Namespace::SessionInfo const> session_ptr)
    : DdlCommand(ddl_data, session_ptr) {}

ExecutionResult ShowDatabasesCommand::execute() {
  // label_infos -> column labels
  std::vector<std::string> labels{"Database", "Owner"};
  std::vector<TargetMetaInfo> label_infos;
  for (const auto& label : labels) {
    label_infos.emplace_back(label, SQLTypeInfo(kTEXT, true));
  }

  // Get all table names
  auto cur_user = session_ptr_->get_currentUser();
  const Catalog_Namespace::DBSummaryList db_summaries =
      Catalog_Namespace::SysCatalog::instance().getDatabaseListForUser(cur_user);

  // logical_values -> table data
  std::vector<RelLogicalValues::RowValues> logical_values;
  for (const auto& db_summary : db_summaries) {
    logical_values.emplace_back(RelLogicalValues::RowValues{});
    logical_values.back().emplace_back(genLiteralStr(db_summary.dbName));
    logical_values.back().emplace_back(genLiteralStr(db_summary.dbOwnerName));
  }

  // Create ResultSet
  std::shared_ptr<ResultSet> rSet = std::shared_ptr<ResultSet>(
      ResultSetLogicalValuesBuilder::create(label_infos, logical_values));

  return ExecutionResult(rSet, label_infos);
}

ShowDiskCacheUsageCommand::ShowDiskCacheUsageCommand(
    const DdlCommandData& ddl_data,
    std::shared_ptr<Catalog_Namespace::SessionInfo const> session_ptr)
    : DdlCommand(ddl_data, session_ptr) {
  auto& ddl_payload = extractPayload(ddl_data_);
  if (ddl_payload.HasMember("tableNames")) {
    CHECK(ddl_payload["tableNames"].IsArray());
    for (auto const& tablename_def : ddl_payload["tableNames"].GetArray()) {
      CHECK(tablename_def.IsString());
    }
  }
}

std::vector<std::string> ShowDiskCacheUsageCommand::getFilteredTableNames() {
  auto table_names = session_ptr_->get_catalog_ptr()->getTableNamesForUser(
      session_ptr_->get_currentUser(), GET_PHYSICAL_TABLES);

  auto& ddl_payload = extractPayload(ddl_data_);
  if (ddl_payload.HasMember("tableNames")) {
    std::vector<std::string> filtered_names;
    for (const auto& tablename_def : ddl_payload["tableNames"].GetArray()) {
      std::string filter_name = tablename_def.GetString();
      if (std::find(table_names.begin(), table_names.end(), filter_name) !=
          table_names.end()) {
        filtered_names.emplace_back(filter_name);
      } else {
        throw std::runtime_error("Can not show disk cache usage for table: " +
                                 filter_name + ". Table does not exist.");
      }
    }
    return filtered_names;
  } else {
    return table_names;
  }
}

ExecutionResult ShowDiskCacheUsageCommand::execute() {
  auto cat_ptr = session_ptr_->get_catalog_ptr();
  auto table_names = getFilteredTableNames();

  const auto disk_cache = cat_ptr->getDataMgr().getPersistentStorageMgr()->getDiskCache();
  if (!disk_cache) {
    throw std::runtime_error{"Disk cache not enabled.  Cannot show disk cache usage."};
  }

  // label_infos -> column labels
  std::vector<std::string> labels{"table name", "current cache size"};
  std::vector<TargetMetaInfo> label_infos;
  label_infos.emplace_back(labels[0], SQLTypeInfo(kTEXT, true));
  label_infos.emplace_back(labels[1], SQLTypeInfo(kBIGINT, true));

  std::vector<RelLogicalValues::RowValues> logical_values;

  for (auto& table_name : table_names) {
    auto [td, td_with_lock] =
        get_table_descriptor_with_lock<lockmgr::ReadLock>(*cat_ptr, table_name, false);

    auto table_cache_size =
        disk_cache->getSpaceReservedByTable(cat_ptr->getDatabaseId(), td->tableId);

    // logical_values -> table data
    logical_values.emplace_back(RelLogicalValues::RowValues{});
    logical_values.back().emplace_back(genLiteralStr(table_name));
    logical_values.back().emplace_back(genLiteralBigInt(table_cache_size));
  }

  std::shared_ptr<ResultSet> rSet = std::shared_ptr<ResultSet>(
      ResultSetLogicalValuesBuilder::create(label_infos, logical_values));

  return ExecutionResult(rSet, label_infos);
}

ShowUserDetailsCommand::ShowUserDetailsCommand(
    const DdlCommandData& ddl_data,
    std::shared_ptr<Catalog_Namespace::SessionInfo const> session_ptr)
    : DdlCommand(ddl_data, session_ptr) {
  auto& ddl_payload = extractPayload(ddl_data);
  if (ddl_payload.HasMember("userNames")) {
    CHECK(ddl_payload["userNames"].IsArray());
    for (const auto& user_name : ddl_payload["userNames"].GetArray()) {
      CHECK(user_name.IsString());
    }
  }
}

ExecutionResult ShowUserDetailsCommand::execute() {
  auto& ddl_payload = extractPayload(ddl_data_);
  auto& sys_cat = Catalog_Namespace::SysCatalog::instance();

  // label_infos -> column labels
  std::vector<std::string> labels{"NAME", "ID", "IS_SUPER", "DEFAULT_DB", "CAN_LOGIN"};
  std::vector<TargetMetaInfo> label_infos;
  label_infos.emplace_back(labels[0], SQLTypeInfo(kTEXT, true));
  label_infos.emplace_back(labels[1], SQLTypeInfo(kBIGINT, true));
  label_infos.emplace_back(labels[2], SQLTypeInfo(kBOOLEAN, true));
  label_infos.emplace_back(labels[3], SQLTypeInfo(kTEXT, true));
  label_infos.emplace_back(labels[4], SQLTypeInfo(kBOOLEAN, true));
  std::vector<RelLogicalValues::RowValues> logical_values;

  Catalog_Namespace::UserMetadata self = session_ptr_->get_currentUser();
  Catalog_Namespace::DBSummaryList dbsums = sys_cat.getDatabaseListForUser(self);
  std::unordered_set<std::string> visible_databases;
  if (!self.isSuper) {
    for (const auto& dbsum : dbsums) {
      visible_databases.insert(dbsum.dbName);
    }
  }

  std::list<Catalog_Namespace::UserMetadata> user_list;
  if (ddl_payload.HasMember("userNames")) {
    for (const auto& user_name_json : ddl_payload["userNames"].GetArray()) {
      std::string user_name = user_name_json.GetString();
      Catalog_Namespace::UserMetadata user;
      if (!sys_cat.getMetadataForUser(user_name, user)) {
        throw std::runtime_error("User with username \"" + user_name +
                                 "\" does not exist. ");
      }
      user_list.emplace_back(std::move(user));
    }
  } else {
    user_list = sys_cat.getAllUserMetadata();
  }

  for (const auto& user : user_list) {
    // database
    std::string dbname;
    Catalog_Namespace::DBMetadata db;
    if (sys_cat.getMetadataForDBById(user.defaultDbId, db)) {
      if (self.isSuper.load() || visible_databases.count(db.dbName)) {
        dbname = db.dbName;
      }
    }
    if (self.isSuper.load()) {
      dbname += "(" + std::to_string(user.defaultDbId) + ")";
    }

    // logical_values -> table data
    logical_values.emplace_back(RelLogicalValues::RowValues{});
    logical_values.back().emplace_back(genLiteralStr(user.userName));
    logical_values.back().emplace_back(genLiteralBigInt(user.userId));
    logical_values.back().emplace_back(genLiteralBoolean(user.isSuper.load()));
    logical_values.back().emplace_back(genLiteralStr(dbname));
    logical_values.back().emplace_back(genLiteralBoolean(user.can_login));
  }

  // Create ResultSet
  std::shared_ptr<ResultSet> rSet = std::shared_ptr<ResultSet>(
      ResultSetLogicalValuesBuilder::create(label_infos, logical_values));

  return ExecutionResult(rSet, label_infos);
}

ReassignOwnedCommand::ReassignOwnedCommand(
    const DdlCommandData& ddl_data,
    std::shared_ptr<Catalog_Namespace::SessionInfo const> session_ptr)
    : DdlCommand(ddl_data, session_ptr) {
  auto& ddl_payload = extractPayload(ddl_data_);
  CHECK(ddl_payload.HasMember("oldOwners"));
  CHECK(ddl_payload["oldOwners"].IsArray());
  for (const auto& old_owner : ddl_payload["oldOwners"].GetArray()) {
    CHECK(old_owner.IsString());
    old_owners_.emplace(old_owner.GetString());
  }
  CHECK(ddl_payload.HasMember("newOwner"));
  CHECK(ddl_payload["newOwner"].IsString());
  new_owner_ = ddl_payload["newOwner"].GetString();
}

ExecutionResult ReassignOwnedCommand::execute() {
  if (!session_ptr_->get_currentUser().isSuper) {
    throw std::runtime_error{
        "Only super users can reassign ownership of database objects."};
  }
  const auto catalog = session_ptr_->get_catalog_ptr();
  catalog->reassignOwners(old_owners_, new_owner_);
  return ExecutionResult();
}
