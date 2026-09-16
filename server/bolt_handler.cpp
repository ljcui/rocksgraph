#include "server/bolt_handler.h"

#include <spdlog/fmt/chrono.h>

#include <boost/algorithm/string.hpp>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "bolt/connection.h"
#include "bolt/path.h"
#include "bolt/spatial.h"
#include "bolt/temporal.h"
#include "bolt/worker_pool.h"
#include "common/exception.h"
#include "common/exceptions.h"
#include "common/logger.h"
#include "graphdb/graph_db.h"
#include "graphdb/transaction.h"
#include "runtime/query_executor.h"
#include "server/bolt_session.h"
#include "server/graph_manager.h"

using namespace bolt;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::steady_clock;
DECLARE_bool(enable_query_log);

namespace bolt {
namespace {}  // namespace

ActiveBoltQuery::~ActiveBoltQuery() { Rollback(); }

void ActiveBoltQuery::Commit() {
  result.reset();
  if (transaction && !transaction_closed) {
    transaction->Commit();
    transaction_closed = true;
  }
  transaction.reset();
}

void ActiveBoltQuery::Rollback() noexcept {
  result.reset();
  if (transaction && !transaction_closed) {
    try {
      transaction->Rollback();
    } catch (const std::exception& e) {
      LOG_WARN("bolt active query rollback failed: {}", e.what());
    } catch (...) {
      LOG_WARN("bolt active query rollback failed with unknown exception");
    }
    transaction_closed = true;
  }
  transaction.reset();
}

}  // namespace bolt

namespace server {

namespace {

struct BoltSessionContext {
  explicit BoltSessionContext(bolt::BoltWorkerPool::Strand strand)
      : session(std::make_shared<BoltSession>()), strand(std::move(strand)) {}

  std::shared_ptr<BoltSession> session;
  bolt::BoltWorkerPool::Strand strand;
};

}  // namespace

namespace {

constexpr std::string_view kDefaultDatabaseName = "default";

rg::LocalDateTime LocalDateTimeFromEpoch(int64_t seconds, int32_t nanoseconds) {
  using namespace std::chrono;
  const sys_seconds point{std::chrono::seconds{seconds}};
  const sys_days day = floor<days>(point);
  const year_month_day date{day};
  const hh_mm_ss time{point - day};
  return {
      {static_cast<int>(date.year()), static_cast<int>(unsigned(date.month())),
       static_cast<int>(unsigned(date.day()))},
      {static_cast<int>(time.hours().count()),
       static_cast<int>(time.minutes().count()),
       static_cast<int>(time.seconds().count()), nanoseconds, true}};
}

int64_t LocalDateTimeToEpochSeconds(const rg::LocalDateTime& value) {
  using namespace std::chrono;
  const auto& date = value.date;
  const auto& time = value.time;
  const sys_days day = year{date.year} /
                       month{static_cast<unsigned>(date.month)} /
                       std::chrono::day{static_cast<unsigned>(date.day)};
  return duration_cast<std::chrono::seconds>(
             day.time_since_epoch() + hours{time.hour} + minutes{time.minute} +
             std::chrono::seconds{time.second})
      .count();
}

rg::Value ConvertParameter(const std::any& data) {
  if (!data.has_value()) {
    return rg::Value::Null();
  }
  const std::type_info& type = data.type();
  if (type == typeid(std::string)) {
    return rg::Value(std::any_cast<const std::string&>(data));
  }
  if (type == typeid(int64_t)) {
    return rg::Value(std::any_cast<int64_t>(data));
  }
  if (type == typeid(double)) {
    return rg::Value(std::any_cast<double>(data));
  }
  if (type == typeid(bool)) {
    return rg::Value(std::any_cast<bool>(data));
  }
  if (type == typeid(std::vector<std::any>)) {
    rg::Value::List result;
    const auto& input = std::any_cast<const std::vector<std::any>&>(data);
    result.reserve(input.size());
    for (const auto& item : input) {
      result.emplace_back(ConvertParameter(item));
    }
    return rg::Value(std::move(result));
  }
  if (type == typeid(std::unordered_map<std::string, std::any>)) {
    rg::Value::Map result;
    const auto& input =
        std::any_cast<const std::unordered_map<std::string, std::any>&>(data);
    for (const auto& [key, value] : input) {
      result.emplace(key, ConvertParameter(value));
    }
    return rg::Value(std::move(result));
  }
  if (type == typeid(bolt::Date)) {
    using namespace std::chrono;
    const year_month_day date{
        sys_days{days{std::any_cast<const bolt::Date&>(data).days}}};
    return rg::Value(rg::Date{static_cast<int>(date.year()),
                              static_cast<int>(unsigned(date.month())),
                              static_cast<int>(unsigned(date.day()))});
  }
  if (type == typeid(bolt::LocalTime)) {
    const int64_t nanos =
        std::any_cast<const bolt::LocalTime&>(data).nanoseconds;
    constexpr int64_t kNanosPerSecond = 1'000'000'000;
    constexpr int64_t kSecondsPerHour = 3600;
    const int64_t seconds = nanos / kNanosPerSecond;
    return rg::Value(
        rg::LocalTime{static_cast<int32_t>(seconds / kSecondsPerHour),
                      static_cast<int32_t>((seconds % kSecondsPerHour) / 60),
                      static_cast<int32_t>(seconds % 60),
                      static_cast<int32_t>(nanos % kNanosPerSecond), true});
  }
  if (type == typeid(bolt::Time)) {
    const auto& input = std::any_cast<const bolt::Time&>(data);
    const rg::Value local = ConvertParameter(
        std::any(bolt::LocalTime{.nanoseconds = input.nanoseconds}));
    return rg::Value(rg::Time{local.AsLocalTime(),
                              static_cast<int32_t>(input.tz_offset_seconds),
                              {}});
  }
  if (type == typeid(bolt::LocalDateTime)) {
    const auto& input = std::any_cast<const bolt::LocalDateTime&>(data);
    return rg::Value(LocalDateTimeFromEpoch(input.seconds, input.nanoseconds));
  }
  if (type == typeid(bolt::DateTime)) {
    const auto& input = std::any_cast<const bolt::DateTime&>(data);
    return rg::Value(rg::DateTime{
        LocalDateTimeFromEpoch(input.seconds + input.tz_offset_seconds,
                               input.nanoseconds),
        static_cast<int32_t>(input.tz_offset_seconds),
        {}});
  }
  if (type == typeid(bolt::LegacyDateTime)) {
    const auto& input = std::any_cast<const bolt::LegacyDateTime&>(data);
    return rg::Value(
        rg::DateTime{LocalDateTimeFromEpoch(input.seconds, input.nanoseconds),
                     static_cast<int32_t>(input.tz_offset_seconds),
                     {}});
  }
  if (type == typeid(bolt::Duration)) {
    const auto& input = std::any_cast<const bolt::Duration&>(data);
    return rg::Value(rg::Duration{input.months, input.days, input.seconds,
                                  static_cast<int32_t>(input.nanos)});
  }
  if (type == typeid(bolt::Point2D)) {
    const auto& input = std::any_cast<const bolt::Point2D&>(data);
    return rg::Value(rg::Point{static_cast<int32_t>(input.spatialRefId),
                               {input.x, input.y}});
  }
  if (type == typeid(bolt::Point3D)) {
    const auto& input = std::any_cast<const bolt::Point3D&>(data);
    return rg::Value(rg::Point{static_cast<int32_t>(input.spatialRefId),
                               {input.x, input.y, input.z}});
  }
  THROW_CODE(InputError, "Unexpected cypher parameter type: {}", type.name());
}

std::unordered_map<std::string, std::any> ConvertMap(
    const rg::Value::Map& values);

std::any ConvertValue(const rg::Value& value) {
  switch (value.Type()) {
    case rg::ValueType::kNull:
      return {};
    case rg::ValueType::kBool:
      return value.AsBool();
    case rg::ValueType::kInteger:
      return value.AsInteger();
    case rg::ValueType::kDouble:
      return value.AsDouble();
    case rg::ValueType::kString:
      return value.AsString();
    case rg::ValueType::kList: {
      std::vector<std::any> result;
      result.reserve(value.AsList().size());
      for (const auto& item : value.AsList()) {
        result.emplace_back(ConvertValue(item));
      }
      return result;
    }
    case rg::ValueType::kMap:
      return ConvertMap(value.AsMap());
    case rg::ValueType::kNode: {
      const auto& node = value.AsNode();
      return bolt::Node{.id = node.id,
                        .elementId = std::to_string(node.id),
                        .labels = node.labels,
                        .props = ConvertMap(node.properties)};
    }
    case rg::ValueType::kRelationship: {
      const auto& relationship = value.AsRelationship();
      return bolt::Relationship{
          .id = relationship.id,
          .elementId = std::to_string(relationship.id),
          .startId = relationship.start_node_id,
          .startElementId = std::to_string(relationship.start_node_id),
          .endId = relationship.end_node_id,
          .endElementId = std::to_string(relationship.end_node_id),
          .type = relationship.type,
          .props = ConvertMap(relationship.properties)};
    }
    case rg::ValueType::kPath: {
      const auto& path = value.AsPath();
      bolt::InternalPath result;
      result.nodes.reserve(path.nodes.size());
      for (const auto& node : path.nodes) {
        if (!node) {
          THROW_CODE(ValueException, "Path contains a null node");
        }
        result.nodes.push_back(
            std::any_cast<bolt::Node>(ConvertValue(rg::Value(node))));
      }
      result.rels.reserve(path.relationships.size());
      result.indices.reserve(path.relationships.size() * 2);
      for (size_t i = 0; i < path.relationships.size(); ++i) {
        const auto& relationship = path.relationships[i];
        if (!relationship || i + 1 >= path.nodes.size()) {
          THROW_CODE(ValueException, "Path has an invalid relationship");
        }
        result.rels.push_back({.id = relationship->id,
                               .elementId = std::to_string(relationship->id),
                               .name = relationship->type,
                               .props = ConvertMap(relationship->properties)});
        const bool forward = relationship->start_node_id == path.nodes[i]->id &&
                             relationship->end_node_id == path.nodes[i + 1]->id;
        result.indices.push_back(forward ? static_cast<int64_t>(i + 1)
                                         : -static_cast<int64_t>(i + 1));
        result.indices.push_back(static_cast<int64_t>(i + 1));
      }
      return result;
    }
    case rg::ValueType::kDate: {
      using namespace std::chrono;
      const auto& date = value.AsDate();
      const sys_days day = year{date.year} /
                           month{static_cast<unsigned>(date.month)} /
                           std::chrono::day{static_cast<unsigned>(date.day)};
      return bolt::Date{day.time_since_epoch().count()};
    }
    case rg::ValueType::kLocalTime: {
      const auto& time = value.AsLocalTime();
      constexpr int64_t kNanosPerSecond = 1'000'000'000;
      const int64_t seconds = time.hour * 3600 + time.minute * 60 + time.second;
      return bolt::LocalTime{seconds * kNanosPerSecond + time.nanosecond};
    }
    case rg::ValueType::kTime: {
      const auto& time = value.AsTime();
      const auto local = std::any_cast<bolt::LocalTime>(
          ConvertValue(rg::Value(time.local_time)));
      return bolt::Time{local.nanoseconds, time.utc_offset_seconds};
    }
    case rg::ValueType::kLocalDateTime: {
      const auto& date_time = value.AsLocalDateTime();
      return bolt::LocalDateTime{LocalDateTimeToEpochSeconds(date_time),
                                 date_time.time.nanosecond};
    }
    case rg::ValueType::kDateTime: {
      const auto& date_time = value.AsDateTime();
      if (!date_time.timezone.empty()) {
        return bolt::DateTimeZoneId{
            LocalDateTimeToEpochSeconds(date_time.local_date_time) -
                date_time.utc_offset_seconds,
            date_time.local_date_time.time.nanosecond, date_time.timezone};
      }
      return bolt::DateTime{
          LocalDateTimeToEpochSeconds(date_time.local_date_time),
          date_time.local_date_time.time.nanosecond,
          date_time.utc_offset_seconds};
    }
    case rg::ValueType::kDuration: {
      const auto& duration = value.AsDuration();
      return bolt::Duration{duration.months, duration.days, duration.seconds,
                            duration.nanoseconds};
    }
    case rg::ValueType::kPoint: {
      const auto& point = value.AsPoint();
      if (point.coordinates.size() == 2) {
        return bolt::Point2D{point.coordinates[0], point.coordinates[1],
                             static_cast<uint32_t>(point.srid)};
      }
      if (point.coordinates.size() == 3) {
        return bolt::Point3D{point.coordinates[0], point.coordinates[1],
                             point.coordinates[2],
                             static_cast<uint32_t>(point.srid)};
      }
      THROW_CODE(ValueException, "Point should have 2 or 3 coordinates");
    }
  }
  THROW_CODE(ValueException, "Unexpected query result value type");
}

std::unordered_map<std::string, std::any> ConvertMap(
    const rg::Value::Map& values) {
  std::unordered_map<std::string, std::any> result;
  for (const auto& [key, value] : values) {
    result.emplace(key, ConvertValue(value));
  }
  return result;
}

std::vector<std::any> ConvertRow(const std::vector<rg::Value>& row) {
  std::vector<std::any> result;
  result.reserve(row.size());
  for (const auto& value : row) {
    result.emplace_back(ConvertValue(value));
  }
  return result;
}

static void FlushSessionBuffer(const std::shared_ptr<BoltConnection>& conn,
                               BoltSession* session) {
  if (session->ps.ConstBuffer().empty()) {
    return;
  }
  conn->PostResponse(std::move(session->ps.MutableBuffer()));
  session->ps.Reset();
}

static void AbortActiveQuery(BoltSession* session) {
  if (session->active_query) {
    session->active_query->Rollback();
    session->active_query.reset();
  }
  session->ps.Reset();
}

static void RequestSessionInterrupt(BoltSession* session) {
  session->RequestInterrupt();
}

static bool ConsumeSessionInterrupt(BoltSession* session) {
  return session->ConsumeInterrupt();
}

static bool IsSessionInterrupted(BoltSession* session) {
  return session->HasInterrupt();
}

static bool IsExplicitTransactionRequest(BoltMsg type) {
  return type == BoltMsg::Begin || type == BoltMsg::Commit ||
         type == BoltMsg::Rollback;
}

static bool IsSessionRequest(BoltMsg type) {
  return type == BoltMsg::Run || type == BoltMsg::PullN ||
         type == BoltMsg::DiscardN || type == BoltMsg::Route ||
         IsExplicitTransactionRequest(type);
}

static bool IsResetMessage(BoltMsg type) { return type == BoltMsg::Reset; }

static std::string_view SessionStateName(SessionState state) {
  switch (state) {
    case SessionState::READY:
      return "READY";
    case SessionState::STREAMING:
      return "STREAMING";
    case SessionState::TX_READY:
      return "TX_READY";
    case SessionState::TX_STREAMING:
      return "TX_STREAMING";
    case SessionState::FAILED:
      return "FAILED";
    case SessionState::DEFUNCT:
      return "DEFUNCT";
  }
  return "UNKNOWN";
}

static void PostSuccess(const std::shared_ptr<BoltConnection>& conn) {
  bolt::PackStream ps;
  ps.AppendSuccess();
  conn->PostResponse(std::move(ps.MutableBuffer()));
}

static void PostIgnored(const std::shared_ptr<BoltConnection>& conn) {
  bolt::PackStream ps;
  ps.AppendIgnored();
  conn->PostResponse(std::move(ps.MutableBuffer()));
}

static void PostFailure(const std::shared_ptr<BoltConnection>& conn,
                        ErrorCode code, const std::string& msg) {
  bolt::PackStream ps;
  ps.AppendFailure({{"code", ErrorCodeToString(code)}, {"message", msg}});
  conn->PostResponse(std::move(ps.MutableBuffer()));
}

static bool InterruptedOrClosed(const std::shared_ptr<BoltConnection>& conn,
                                BoltSession* session) {
  if (conn->has_closed()) {
    LOG_INFO("The bolt connection is closed, cancel the op execution.");
    return true;
  }
  if (IsSessionInterrupted(session)) {
    LOG_INFO("The bolt session is interrupted, cancel the op execution.");
    return true;
  }
  return false;
}

static void FailSession(const std::shared_ptr<BoltConnection>& conn,
                        BoltSession* session, ErrorCode code,
                        const std::string& msg) {
  AbortActiveQuery(session);
  PostFailure(conn, code, msg);
  session->state = SessionState::FAILED;
}

static void CloseProtocolError(const std::shared_ptr<BoltConnection>& conn,
                               BoltSession* session, BoltMsg type) {
  LOG_ERROR("Unexpected msg:{} in {} state, close the connection",
            ToString(type), SessionStateName(session->state));
  AbortActiveQuery(session);
  session->state = SessionState::DEFUNCT;
  conn->Close();
}

static void ProcessRecoverableState(const std::shared_ptr<BoltConnection>& conn,
                                    BoltSession* session, BoltMsg type) {
  if (IsSessionRequest(type)) {
    PostIgnored(conn);
  } else {
    CloseProtocolError(conn, session, type);
  }
}

static void FailUnsupportedRequest(const std::shared_ptr<BoltConnection>& conn,
                                   BoltSession* session, BoltMsg type,
                                   std::string_view feature) {
  std::string err = fmt::format(
      "The {} feature is not currently supported for Bolt connections.",
      feature);
  LOG_ERROR("Receive {}, but {}", ToString(type), err);
  FailSession(conn, session, ErrorCode::Unimplemented, err);
}

static int64_t ExtractPullOrDiscardN(BoltMsg type,
                                     const std::vector<std::any>& fields) {
  if (fields.size() != 1) {
    THROW_CODE(InputError, "{} msg fields size error, size: {}",
               bolt::ToString(type), fields.size());
  }
  auto* metadata =
      std::any_cast<std::unordered_map<std::string, std::any>>(&fields[0]);
  if (metadata == nullptr) {
    THROW_CODE(InputError, "{} metadata should be a map", bolt::ToString(type));
  }
  auto iter = metadata->find("n");
  if (iter == metadata->end()) {
    THROW_CODE(InputError, "{} metadata should contain n",
               bolt::ToString(type));
  }
  auto* n = std::any_cast<int64_t>(&iter->second);
  if (n == nullptr) {
    THROW_CODE(InputError, "{} n should be an integer", bolt::ToString(type));
  }
  if (*n == 0) {
    THROW_CODE(InputError, "{} n should not be 0", bolt::ToString(type));
  }
  return *n;
}

static void ProcessPullOrDiscard(const std::shared_ptr<BoltConnection>& conn,
                                 BoltSession* session, BoltMsg type,
                                 const std::vector<std::any>& fields) {
  try {
    if (!session->active_query || !session->active_query->result) {
      THROW_CODE(InputError, "{} requires an active result stream",
                 bolt::ToString(type));
    }

    const int64_t n = ExtractPullOrDiscardN(type, fields);
    const bool unlimited = n < 0;
    int64_t remaining = n;
    auto* active_query = session->active_query.get();

    while (unlimited || remaining > 0) {
      if (InterruptedOrClosed(conn, session)) {
        AbortActiveQuery(session);
        return;
      }

      std::vector<rg::Value> row;
      bool has_row = false;
      if (active_query->buffered_row.has_value()) {
        row = std::move(*active_query->buffered_row);
        active_query->buffered_row.reset();
        has_row = true;
      } else {
        has_row = active_query->result->Next(&row);
      }
      if (!has_row) {
        break;
      }
      if (type == BoltMsg::PullN) {
        session->ps.AppendRecord(ConvertRow(row));
        if (session->ps.ConstBuffer().size() > 1024) {
          FlushSessionBuffer(conn, session);
        }
      }
      if (!unlimited) {
        --remaining;
      }
    }

    if (!unlimited && remaining == 0) {
      std::vector<rg::Value> next_row;
      if (active_query->result->Next(&next_row)) {
        active_query->buffered_row.emplace(std::move(next_row));
      }
    }

    if (active_query->buffered_row.has_value()) {
      session->ps.AppendSuccessHasMore(true);
      session->state = SessionState::STREAMING;
      FlushSessionBuffer(conn, session);
      return;
    }

    auto elapsed = duration_cast<milliseconds>(steady_clock::now() -
                                               active_query->start_time);
    auto graph_name = active_query->graph_name;
    auto cypher = active_query->cypher;
    active_query->Commit();
    session->active_query.reset();
    session->state = SessionState::READY;
    session->ps.AppendSuccess();
    FlushSessionBuffer(conn, session);
    LOG_DEBUG("Cypher execution completed");
    QUERY_LOG("{} {} {}", graph_name, elapsed, cypher.substr(0, 256));
  } catch (const RocksGraphException& e) {
    LOG_ERROR("{}", e.msg());
    FailSession(conn, session, e.code(), e.msg());
  } catch (std::exception& e) {
    LOG_ERROR("{}", e.what());
    FailSession(conn, session, ErrorCode::UnknownError, e.what());
  }
}

static void ProcessRun(GraphManager* graph_manager,
                       const std::shared_ptr<BoltConnection>& conn,
                       BoltSession* session, std::vector<std::any>& fields) {
  try {
    if (fields.size() != 3) {
      THROW_CODE(InputError, "Run msg fields size error, size: {}",
                 fields.size());
    }
    auto* cypher = std::any_cast<std::string>(&fields[0]);
    auto* params =
        std::any_cast<std::unordered_map<std::string, std::any>>(&fields[1]);
    auto* extra =
        std::any_cast<std::unordered_map<std::string, std::any>>(&fields[2]);
    if (cypher == nullptr || params == nullptr || extra == nullptr) {
      THROW_CODE(InputError,
                 "Run msg fields should be (string, map, map), got ({}, {}, "
                 "{})",
                 fields[0].type().name(), fields[1].type().name(),
                 fields[2].type().name());
    }

    std::string graph;
    auto db_iter = extra->find("db");
    if (db_iter != extra->end()) {
      auto* db = std::any_cast<std::string>(&db_iter->second);
      if (db == nullptr) {
        THROW_CODE(InputError, "Run msg db metadata should be a string");
      }
      graph = *db;
    }
    if (graph.empty()) {
      graph = kDefaultDatabaseName;
    }

    auto active_query = std::make_unique<ActiveBoltQuery>();
    active_query->graph_name = graph;
    active_query->cypher = *cypher;
    active_query->start_time = steady_clock::now();
    rg::QueryOptions query_options;
    for (const auto& [name, value] : *params) {
      query_options.parameters.emplace(name, ConvertParameter(value));
    }
    active_query->graph_db = graph_manager->OpenGraph(graph);
    active_query->transaction = active_query->graph_db->BeginTransaction();
    LOG_DEBUG("Execute {}", active_query->cypher.substr(0, 256));
    active_query->result =
        rg::ExecuteQueryCursor(*active_query->transaction, active_query->cypher,
                               std::move(query_options));

    bolt::PackStream ps;
    ps.AppendSuccessFields(active_query->result->Columns());
    conn->PostResponse(std::move(ps.MutableBuffer()));
    session->active_query = std::move(active_query);
    session->state = bolt::SessionState::STREAMING;
  } catch (const RocksGraphException& e) {
    LOG_ERROR("{}", e.msg());
    FailSession(conn, session, e.code(), e.msg());
  } catch (std::exception& e) {
    LOG_ERROR("{}", e.what());
    FailSession(conn, session, ErrorCode::UnknownError, e.what());
  }
}

static void ProcessReadyState(GraphManager* graph_manager,
                              const std::shared_ptr<BoltConnection>& conn,
                              BoltSession* session, BoltMsg type,
                              std::vector<std::any>& fields) {
  if (IsExplicitTransactionRequest(type)) {
    FailUnsupportedRequest(conn, session, type, "explicit transactions");
  } else if (type == bolt::BoltMsg::Route) {
    FailUnsupportedRequest(conn, session, type, "routing");
  } else if (type == bolt::BoltMsg::Run) {
    ProcessRun(graph_manager, conn, session, fields);
  } else {
    CloseProtocolError(conn, session, type);
  }
}

static void ProcessStreamingState(const std::shared_ptr<BoltConnection>& conn,
                                  BoltSession* session, BoltMsg type,
                                  const std::vector<std::any>& fields) {
  if (type == bolt::BoltMsg::PullN || type == bolt::BoltMsg::DiscardN) {
    ProcessPullOrDiscard(conn, session, type, fields);
  } else {
    CloseProtocolError(conn, session, type);
  }
}

static void ProcessBoltMessage(GraphManager* graph_manager,
                               const std::shared_ptr<BoltConnection>& conn,
                               BoltSession* session, BoltMsgDetail msg) {
  auto& fields = msg.fields;
  auto type = msg.type;

  if (IsSessionInterrupted(session)) {
    AbortActiveQuery(session);
    if (IsResetMessage(type)) {
      if (ConsumeSessionInterrupt(session)) {
        session->state = SessionState::READY;
        PostSuccess(conn);
      } else {
        PostIgnored(conn);
      }
    } else {
      PostIgnored(conn);
    }
    return;
  }

  switch (session->state) {
    case SessionState::FAILED:
      ProcessRecoverableState(conn, session, type);
      break;
    case SessionState::READY:
      ProcessReadyState(graph_manager, conn, session, type, fields);
      break;
    case SessionState::STREAMING:
      ProcessStreamingState(conn, session, type, fields);
      break;
    case SessionState::TX_READY:
    case SessionState::TX_STREAMING:
    case SessionState::DEFUNCT:
      CloseProtocolError(conn, session, type);
      break;
  }
}

static std::shared_ptr<BoltSessionContext> GetSessionContext(
    BoltConnection& conn) {
  auto ctx = conn.GetContextShared();
  if (!ctx) {
    return {};
  }
  return std::static_pointer_cast<BoltSessionContext>(ctx);
}

static bool EnqueueSessionMessage(
    GraphManager* graph_manager,
    const std::shared_ptr<bolt::BoltWorkerPool>& pool, BoltConnection& conn,
    std::shared_ptr<BoltSessionContext> context, BoltMsgDetail msg) {
  if (!pool->Post(
          context->strand, [graph_manager, conn = conn.shared_from_this(),
                            context, msg = std::move(msg)]() mutable {
            if (!conn->has_closed()) {
              ProcessBoltMessage(graph_manager, conn, context->session.get(),
                                 std::move(msg));
            }
            if (conn->has_closed()) {
              AbortActiveQuery(context->session.get());
            }
          })) {
    LOG_WARN("failed to schedule bolt session: worker pool is stopped");
    conn.Close();
    return false;
  }
  return true;
}

}  // namespace

BoltHandler NewBoltHandler(GraphManager* graph_manager,
                           BoltHandlerOptions options) {
  auto worker_pool = std::make_shared<bolt::BoltWorkerPool>(
      options.worker_thread_num, "bolt-worker-", "bolt");
  return [graph_manager, options, worker_pool](
             BoltConnection& conn, BoltMsg msg, std::vector<std::any> fields) {
    if (msg == BoltMsg::Hello) {
      auto existing_context = GetSessionContext(conn);
      if (existing_context) {
        LOG_WARN("receive duplicate Bolt HELLO, close the connection");
        conn.Close();
        return;
      }
      if (fields.size() != 1) {
        LOG_ERROR("Hello msg fields size error, size: {}", fields.size());
        bolt::PackStream ps;
        ps.AppendFailure(
            {{"code", "error"}, {"message", "Hello msg fields size error"}});
        conn.Respond(std::move(ps.MutableBuffer()));
        conn.Close();
        return;
      }
      auto* val =
          std::any_cast<std::unordered_map<std::string, std::any>>(&fields[0]);
      if (val == nullptr) {
        std::string err = "Hello msg metadata should be a map";
        LOG_ERROR(err);
        bolt::PackStream ps;
        ps.AppendFailure({{"code", "error"}, {"message", err}});
        conn.Respond(std::move(ps.MutableBuffer()));
        conn.Close();
        return;
      }
      auto principal_iter = val->find("principal");
      auto credentials_iter = val->find("credentials");
      if (principal_iter == val->end() || credentials_iter == val->end()) {
        std::string err = "Miss 'principal' or 'credentials' in Hello msg";
        LOG_ERROR(err);
        bolt::PackStream ps;
        ps.AppendFailure({{"code", "error"}, {"message", err}});
        conn.Respond(std::move(ps.MutableBuffer()));
        conn.Close();
        return;
      }
      auto* principal = std::any_cast<std::string>(&principal_iter->second);
      auto* credentials = std::any_cast<std::string>(&credentials_iter->second);
      if (principal == nullptr || credentials == nullptr) {
        std::string err =
            "'principal' and 'credentials' in Hello msg should be strings";
        LOG_ERROR(err);
        bolt::PackStream ps;
        ps.AppendFailure({{"code", "error"}, {"message", err}});
        conn.Respond(std::move(ps.MutableBuffer()));
        conn.Close();
        return;
      }
      (void)credentials;
      /* TODO(anyone): wire real authentication through the server-owned graph
       * manager.
       */
      std::unordered_map<std::string, std::any> meta;
      meta["connection_id"] =
          std::string("bolt") + std::to_string(conn.conn_id());
      // Neo4j python client check that the returned server info must start
      // with 'Neo4j/'
      meta["server"] = "Neo4j/rocksgraph";
      auto context =
          std::make_shared<BoltSessionContext>(worker_pool->MakeStrand());
      auto session = context->session;
      auto user_agent_iter = val->find("user_agent");
      if (user_agent_iter != val->end()) {
        auto* user_agent = std::any_cast<std::string>(&user_agent_iter->second);
        if (user_agent != nullptr &&
            boost::algorithm::starts_with(*user_agent, "neo4j-python")) {
          session->python_driver = true;
        }
      }
      auto patch_iter = val->find("patch_bolt");
      if (patch_iter != val->end()) {
        auto* patch = std::any_cast<std::vector<std::any>>(&patch_iter->second);
        if (patch != nullptr && patch->size() == 1) {
          auto* item = std::any_cast<std::string>(&(*patch)[0]);
          if (item != nullptr && *item == "utc") {
            session->utc_patch = true;
            meta["patch_bolt"] = std::vector<std::string>{"utc"};
          }
        }
      }
      session->state = SessionState::READY;
      session->user = *principal;
      conn.SetContext(context);
      bolt::PackStream ps;
      ps.AppendSuccess(meta);
      conn.Respond(std::move(ps.MutableBuffer()));
    } else if (msg == BoltMsg::Goodbye) {
      conn.Close();
      return;
    } else if (msg == BoltMsg::Run || msg == BoltMsg::PullN ||
               msg == BoltMsg::DiscardN || msg == BoltMsg::Begin ||
               msg == BoltMsg::Commit || msg == BoltMsg::Rollback ||
               msg == BoltMsg::Route || msg == BoltMsg::Reset) {
      auto context = GetSessionContext(conn);
      if (!context) {
        LOG_WARN("receive {} before Bolt HELLO, close the connection",
                 ToString(msg));
        conn.Close();
        return;
      }
      if (msg == BoltMsg::Reset) {
        RequestSessionInterrupt(context->session.get());
      }
      EnqueueSessionMessage(graph_manager, worker_pool, conn,
                            std::move(context), {msg, std::move(fields)});
    } else {
      LOG_WARN("receive unknown bolt message: {}", ToString(msg));
      conn.Close();
    }
  };
}
}  // namespace server
