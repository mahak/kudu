// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/master/hms_notification_log_listener.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <rapidjson/error/en.h>

#include "kudu/gutil/macros.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/hms/hive_metastore_types.h"
#include "kudu/hms/hms_catalog.h"
#include "kudu/hms/hms_client.h"
#include "kudu/master/catalog_manager.h"
#include "kudu/util/async_util.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/monotime.h"
#include "kudu/util/slice.h"
#include "kudu/util/status_callback.h"
#include "kudu/util/thread.h"
#include "kudu/util/url-coding.h"
#include "kudu/util/zlib.h"

DEFINE_uint32(hive_metastore_notification_log_poll_period_seconds, 15,
              "Amount of time the notification log listener waits between attempts to poll "
              "the Hive Metastore for catalog updates.");
TAG_FLAG(hive_metastore_notification_log_poll_period_seconds, advanced);
TAG_FLAG(hive_metastore_notification_log_poll_period_seconds, runtime);

DEFINE_int32(hive_metastore_notification_log_batch_size, 100,
             "Number of notification log entries which are retrieved from the Hive Metastore "
             "per batch when polling.");
TAG_FLAG(hive_metastore_notification_log_batch_size, advanced);
TAG_FLAG(hive_metastore_notification_log_batch_size, runtime);

DEFINE_uint32(hive_metastore_notification_log_poll_inject_latency_ms, 0,
              "Inject latency into the inner polling loop of the Hive Metastore "
              "notification log listener. Only takes effect during unit tests.");
TAG_FLAG(hive_metastore_notification_log_poll_inject_latency_ms, hidden);
TAG_FLAG(hive_metastore_notification_log_poll_inject_latency_ms, unsafe);
TAG_FLAG(hive_metastore_notification_log_poll_inject_latency_ms, runtime);

DEFINE_int32(hive_metastore_notification_log_listener_catch_up_deadline_ms, 30000,
  "The deadline in milliseconds for the HMS log listener to catch up with the "
  "latest log entry.");
TAG_FLAG(hive_metastore_notification_log_listener_catch_up_deadline_ms, advanced);
TAG_FLAG(hive_metastore_notification_log_listener_catch_up_deadline_ms, experimental);
TAG_FLAG(hive_metastore_notification_log_listener_catch_up_deadline_ms, runtime);

using rapidjson::Document;
using rapidjson::Value;
using std::optional;
using std::ostringstream;
using std::string;
using std::vector;
using strings::Substitute;

namespace kudu {
namespace master {

// Status message returned when the task is shutdown.
static const char* kShutdownMessage =
  "Hive Metastore notification log listener is shutting down";

HmsNotificationLogListenerTask::HmsNotificationLogListenerTask(CatalogManager* catalog_manager)
  : catalog_manager_(catalog_manager),
    closing_(false),
    wake_up_cv_(&lock_) {
}

HmsNotificationLogListenerTask::~HmsNotificationLogListenerTask() {
  if (thread_) {
    Shutdown();
  }
}

Status HmsNotificationLogListenerTask::Init() {
  CHECK(!thread_) << "HmsNotificationLogListenerTask is already initialized";
  return kudu::Thread::Create("catalog manager", "hms-notification-log-listener",
                              [this]() { this->RunLoop(); }, &thread_);
}

void HmsNotificationLogListenerTask::Shutdown() {
  CHECK(thread_) << "HmsNotificationLogListenerTask is not initialized";
  {
    std::lock_guard l(lock_);
    DCHECK(!closing_);
    closing_ = true;
    wake_up_cv_.Signal();
  }
  CHECK_OK(ThreadJoiner(thread_.get()).Join());
  thread_.reset();
}

Status HmsNotificationLogListenerTask::WaitForCatchUp(const MonoTime& deadline) {
  Synchronizer synchronizer;
  {
    std::lock_guard l(lock_);
    if (closing_) {
      return Status::ServiceUnavailable(kShutdownMessage);
    }
    catch_up_callbacks_.emplace_back(synchronizer.AsStatusCallback());
    wake_up_cv_.Signal();
  }

  RETURN_NOT_OK_PREPEND(synchronizer.WaitUntil(deadline),
                        "failed to wait for Hive Metastore notification log listener to catch up");
  return Status::OK();
}

void HmsNotificationLogListenerTask::RunLoop() {
  vector<StatusCallback> callback_batch;
  while (true) {
    Status s = Poll();
    WARN_NOT_OK(s, "Hive Metastore notification log listener poll failed");

    // Wakeup all threads which enqueued before beginning the poll.
    for (auto& cb : callback_batch) {
      cb(s);
    }
    callback_batch.clear();

    {
      std::lock_guard l(lock_);

      // Check if shutdown was signaled while polling.
      if (closing_) {
        callback_batch = std::move(catch_up_callbacks_);
        break;
      }

      // Check if a waiter thread enqueued while polling. If not, then wait for
      // up to a poll period to elapse.
      if (catch_up_callbacks_.empty()) {
        wake_up_cv_.WaitFor(
          MonoDelta::FromSeconds(FLAGS_hive_metastore_notification_log_poll_period_seconds));
      }

      // Swap the current queue of callbacks, so they can be completed after
      // polling next iteration.
      callback_batch.swap(catch_up_callbacks_);

      // Check if shutdown was signaled while waiting.
      if (closing_) {
        break;
      }
    }
  }

  for (auto& cb : callback_batch) {
    cb(Status::ServiceUnavailable(kShutdownMessage));
  }
}

namespace {

// Returns a text string appropriate for debugging a notification event.
string EventDebugString(const hive::NotificationEvent& event) {
  return Substitute("$0 $1 $2.$3", event.eventId, event.eventType, event.dbName, event.tableName);
}

// Deserializes an HMS table object from a JSON notification log message.
Status DeserializeTable(const hive::NotificationEvent(event),
                        const Document& message,
                        const char* key,
                        hive::Table* table) {
  if (!message.HasMember(key)) {
    return Status::Corruption("field is not present", key);
  }
  if (!message[key].IsString()) {
    return Status::Corruption("field is not a string", key);
  }

  const Value& value = message[key];
  Slice slice(value.GetString(), value.GetStringLength());
  return hms::HmsClient::DeserializeJsonTable(slice, table);
}
} // anonymous namespace

Status HmsNotificationLogListenerTask::Poll() {
  if (!catalog_manager_) {
    SleepFor(MonoDelta::FromMilliseconds(
          FLAGS_hive_metastore_notification_log_poll_inject_latency_ms));
    // Unit-test mode.
    return Status::OK();
  }

  // This method calls the catalog manager directly, so ensure the leader lock is held.
  CatalogManager::ScopedLeaderSharedLock l(catalog_manager_);
  if (!l.first_failed_status().ok()) {
    VLOG(1) << "Skipping Hive Metastore notification log poll: "
              << l.first_failed_status().ToString();
    return Status::OK();
  }

  // Cache the batch size, since it's a runtime flag.
  int32_t batch_size = FLAGS_hive_metastore_notification_log_batch_size;

  // Retrieve the last processed event ID from the catalog manager. The latest
  // event ID is requested for every call to Poll() because leadership may have
  // changed, and another leader may have processed events.
  int64_t durable_event_id = catalog_manager_->GetLatestNotificationLogEventId();

  // Also keep track of the latest event ID which has been processed locally.
  int64_t processed_event_id = durable_event_id;
  vector<hive::NotificationEvent> events;
  while (true) {
    events.clear();

    {
      std::lock_guard l(lock_);
      if (closing_) {
        return Status::ServiceUnavailable(kShutdownMessage);
      }
    }

    RETURN_NOT_OK_PREPEND(catalog_manager_->hms_catalog()->GetNotificationEvents(
        processed_event_id, batch_size, &events),
                          "failed to retrieve notification log events");

    // If we do not receive any new events it could be because the HMS event ID in the Kudu
    // master is higher than what is in the HMS database which causes Drop/Alter table
    // commands to fail on Kudu side.
    if (events.empty()) {
      int64_t event_id;
          RETURN_NOT_OK_PREPEND(catalog_manager_->hms_catalog()->
          GetCurrentNotificationEventId(&event_id),
                                "failed to retrieve latest notification log event");
      if (event_id < processed_event_id) {
        LOG(ERROR) << Substitute("The event ID $0 last seen by Kudu master is greater "
                                 "than $1 currently reported by HMS. Has the HMS database "
                                 "been reset (backup&restore, etc.)?",
                                 processed_event_id, event_id);
      }
    }

#if DCHECK_IS_ON()
    {
      int64_t last_seen_event_id = std::numeric_limits<int64_t>::min();
      for (size_t idx = 0; idx < events.size(); ++idx) {
        const auto event_id = events[idx].eventId;
        DCHECK_GT(event_id, std::numeric_limits<int64_t>::min());
        if (event_id > last_seen_event_id) {
          last_seen_event_id = event_id;
          continue;
        }
        // Print out diagnostic information into the logs.
        DCHECK_GT(idx, 0);
        string msg = Substitute(
            "non-monotonous event IDs from HMS: current $0, previous $1; "
            "dumping first $2 out of $3 received events:",
            event_id, events[idx - 1].eventId, idx + 1, events.size());
        ostringstream events_str;
        for (size_t j = 0; j <= idx; ++j) {
          events_str << " ";
          events[j].printTo(events_str);
          events_str << ";";
        }
        LOG(DFATAL) << msg << events_str.str();
      }
    }
#endif // #if DCHECK_IS_ON() ...

    for (const auto& event : events) {
      VLOG(1) << "Processing notification log event: " << EventDebugString(event);

      // Check for out-of-order events. Out-of-order events are skipped, since
      // refusing to process them by returning early would result in the
      // notification log listener indefinitely short-circuiting on the same
      // invalid event.
      if (event.eventId <= processed_event_id) {
        LOG(DFATAL) << "Received out-of-order notification log event "
                    << "(last processed event ID: " << processed_event_id << "): "
                    << EventDebugString(event);
        continue;
      }

      Status s;
      if (event.eventType == "ALTER_TABLE") {
        s = HandleAlterTableEvent(event, &durable_event_id);
      } else if (event.eventType == "DROP_TABLE") {
        s = HandleDropTableEvent(event, &durable_event_id);
      }

      // Failing to properly handle a notification is not a fatal error, instead
      // we continue processing notifications. Callers of WaitForCatchUp have no
      // way of indicating which specific notification they are waiting for, and
      // returning early with error pertaining to a different notifications
      // could result in not waiting long enough.
      //
      // Consider a CREATE TABLE call which succeeds in adding an entry to the
      // HMS, but fails to write to the sys catalog, because leadership has been
      // lost. In this case a rollback attempt will occur, and the entry will be
      // deleted from the HMS. When the notification for that delete is
      // processed by the listener, it will necessarily fail to apply, since the
      // table never existed in Kudu. It's critical that in cases like this
      // the notification log listener continues to make progress.
      //
      // TODO(KUDU-2475): Ignoring errors could result in a client receiving an
      // ack for a table rename or drop which fails.
      WARN_NOT_OK(s, Substitute("Failed to handle Hive Metastore notification: $0",
                                 EventDebugString(event)));

      // Short-circuit when leadership is lost to prevent applying notification
      // events out of order.
      if (l.has_term_changed()) {
        return Status::ServiceUnavailable(
            "lost leadership while handling Hive Metastore notification log events", s.message());
      }

      processed_event_id = event.eventId;
    }

    // If the last set of events was smaller than the batch size then we can
    // assume that we've read all of the available events.
    if (events.size() < batch_size) break;
  }

  // The durable event ID gets updated every time we make a change in response
  // to a log notification, however not every log notification results in a
  // change (for instance, a notification pertaining to a Parquet table). To
  // avoid replaying these notifications we persist the latest processed
  // notification log event ID after polling. This is best effort, since failing
  // to update the ID should only results in wasted work, not an unsynchronized
  // catalog.
  if (durable_event_id < processed_event_id) {
    WARN_NOT_OK(catalog_manager_->StoreLatestNotificationLogEventId(processed_event_id),
                "failed to record latest processed Hive Metastore notification log ID");
  }

  return Status::OK();
}

Status HmsNotificationLogListenerTask::HandleAlterTableEvent(const hive::NotificationEvent& event,
                                                             int64_t* durable_event_id) {
  Document message;
  RETURN_NOT_OK(ParseMessage(event, &message));

  hive::Table before_table;
  RETURN_NOT_OK(DeserializeTable(event, message, "tableObjBeforeJson", &before_table));

  if (!hms::HmsClient::IsSynchronized(before_table)) {
    // Not a synchronized table; skip it.
    VLOG(2) << Substitute("Ignoring alter event for table $0 of type $1",
                          before_table.tableName, before_table.tableType);
    return Status::OK();
  }

  const string* storage_handler =
      FindOrNull(before_table.parameters, hms::HmsClient::kStorageHandlerKey);

  if (!hms::HmsClient::IsKuduTable(before_table)) {
    // Not a Kudu table; skip it.
    VLOG(2) << Substitute("Ignoring alter event for non-Kudu table $0",
                          before_table.tableName);
    return Status::OK();
  }

  // If there is not a cluster ID, for maximum compatibility we should assume this is an older
  // Kudu table without a cluster ID set. This is safe because we still validate the table ID
  // which is universally unique.
  const string* cluster_id =
      FindOrNull(before_table.parameters, hms::HmsClient::kKuduClusterIdKey);
  if (cluster_id && *cluster_id != catalog_manager_->GetClusterId()) {
    // Not for this cluster; skip it.
    VLOG(2) << Substitute("Ignoring alter event for table $0 of cluster $1",
        before_table.tableName, *cluster_id);
    return Status::OK();
  }

  hive::Table after_table;
  RETURN_NOT_OK(DeserializeTable(event, message, "tableObjAfterJson", &after_table));

  // Double check that the Kudu HMS plugin is enforcing storage handler and
  // table ID constraints correctly.
  const string* after_storage_handler =
      FindOrNull(before_table.parameters, hms::HmsClient::kStorageHandlerKey);
  if (!after_storage_handler || *after_storage_handler != *storage_handler) {
    return Status::IllegalState("storage handler property altered");
  }

  const string* table_id = FindOrNull(before_table.parameters, hms::HmsClient::kKuduTableIdKey);
  if (!table_id) {
    return Status::IllegalState("missing Kudu table ID");
  }
  const string* after_table_id = FindOrNull(after_table.parameters,
                                            hms::HmsClient::kKuduTableIdKey);
  if (!after_table_id || *after_table_id != *table_id) {
    return Status::IllegalState("Kudu table ID altered");
  }

  string before_table_name = Substitute("$0.$1", before_table.dbName, before_table.tableName);
  string after_table_name = Substitute("$0.$1", event.dbName, event.tableName);

  optional<string> new_table_name;
  if (before_table_name != after_table_name) {
    new_table_name = after_table_name;
  }

  optional<string> new_table_owner;
  if (before_table.owner != after_table.owner) {
    new_table_owner = after_table.owner;
  }

  static const string kDefaultValue = "";
  const auto& before_table_comment = FindWithDefault(before_table.parameters,
                                                     hms::HmsClient::kTableCommentKey,
                                                     kDefaultValue);
  const auto& after_table_comment = FindWithDefault(after_table.parameters,
                                                    hms::HmsClient::kTableCommentKey,
                                                    kDefaultValue);
  optional<string> new_table_comment;
  if (before_table_comment != after_table_comment) {
    new_table_comment.emplace(after_table_comment);
  }
  if (!new_table_name && !new_table_owner && !new_table_comment) {
    VLOG(2) << "Ignoring alter table event on table "
            << *table_id << " " << before_table_name;
    return Status::OK();
  }

  RETURN_NOT_OK(catalog_manager_->AlterTableHms(*table_id,
                                                before_table_name,
                                                new_table_name,
                                                new_table_owner,
                                                new_table_comment,
                                                event.eventId));
  *durable_event_id = event.eventId;
  return Status::OK();
}

Status HmsNotificationLogListenerTask::HandleDropTableEvent(const hive::NotificationEvent& event,
                                                            int64_t* durable_event_id) {
  Document message;
  RETURN_NOT_OK(ParseMessage(event, &message));

  hive::Table table;
  RETURN_NOT_OK(DeserializeTable(event, message, "tableObjJson", &table));

  if (!hms::HmsClient::IsSynchronized(table)) {
    // Not a synchronized table; skip it.
    VLOG(2) << Substitute("Ignoring drop event for table $0 of type $1",
                          table.tableName, table.tableType);
    return Status::OK();
  }

  if (!hms::HmsClient::IsKuduTable(table)) {
    // Not a Kudu table; skip it.
    VLOG(2) << Substitute("Ignoring drop event for non-Kudu table $0", table.tableName);
    return Status::OK();
  }

  // If there is not a cluster ID, for maximum compatibility we should assume this is an older
  // Kudu table without a cluster ID set. This is safe because we still validate the table ID
  // which is universally unique.
  const string* cluster_id =
      FindOrNull(table.parameters, hms::HmsClient::kKuduClusterIdKey);
  if (cluster_id && *cluster_id != catalog_manager_->GetClusterId()) {
    // Not for this cluster; skip it.
    VLOG(2) << Substitute("Ignoring alter event for table $0 of cluster $1",
                          table.tableName, *cluster_id);
    return Status::OK();
  }

  const string* table_id = FindOrNull(table.parameters, hms::HmsClient::kKuduTableIdKey);
  if (!table_id) {
    return Status::IllegalState("missing Kudu table ID");
  }

  // Require the table ID *and* table name from the HMS drop event to match the
  // Kudu catalog's metadata for the table. Checking the name in addition to the
  // ID prevents a table from being dropped while the HMS and Kudu catalogs are
  // unsynchronized. If the catalogs are unsynchronized, it's better to return
  // an error than liberally delete data.
  string table_name = Substitute("$0.$1", event.dbName, event.tableName);
  RETURN_NOT_OK(catalog_manager_->DeleteTableHms(table_name, *table_id, event.eventId));
  *durable_event_id = event.eventId;
  return Status::OK();
}

Status HmsNotificationLogListenerTask::ParseMessage(const hive::NotificationEvent& event,
                                                    Document* message) {
  string format = event.messageFormat;
  // Default to the json-0.2 format for backwards compatibility.
  if (format.empty()) {
    format = "json-0.2";
  }

  // See Hive's JSONMessageEncoder and GzipJSONMessageEncoder for the format definitions.
  if (event.messageFormat != "json-0.2" && event.messageFormat != "gzip(json-2.0)") {
    return Status::NotSupported("unknown message format", event.messageFormat);
  }

  string content = event.message;
  if (HasPrefixString(format, "gzip")) {
    string decoded;
    KUDU_RETURN_NOT_OK(DecodeGzipMessage(content, &decoded));
    content = decoded;
  }

  if (message->Parse<0>(content.c_str()).HasParseError()) {
    return Status::Corruption("failed to parse message",
                              rapidjson::GetParseError_En(message->GetParseError()));
  }

  return Status::OK();
}

Status HmsNotificationLogListenerTask::DecodeGzipMessage(const string& encoded,
                                                         string* decoded) {
  string result;
  bool success = Base64Decode(encoded, &result);
  if (!success) {
    return Status::Corruption("failed to decode message");
  }
  std::ostringstream oss;
  RETURN_NOT_OK_PREPEND(zlib::Uncompress(Slice(result), &oss), "failed decompress message");
  *decoded = oss.str();
  return Status::OK();
}

} // namespace master
} // namespace kudu
