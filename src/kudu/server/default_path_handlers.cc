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

#include "kudu/server/default_path_handlers.h"

#include <sys/stat.h>
// IWYU pragma: no_include <bits/struct_stat.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "kudu/util/prometheus_writer.h"
#include "kudu/util/version_info.h"
#include "kudu/util/version_info.pb.h"

#ifdef TCMALLOC_ENABLED
#include <boost/algorithm/string/replace.hpp>
#include <boost/iterator/iterator_traits.hpp>
#include <gperftools/malloc_extension.h>
#endif

#include "kudu/gutil/macros.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/human_readable.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/gutil/strings/split.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/server/pprof_path_handlers.h"
#include "kudu/server/webserver.h"
#include "kudu/util/array_view.h"
#include "kudu/util/debug-util.h"
#include "kudu/util/easy_json.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/flags.h"
#include "kudu/util/jsonwriter.h"
#include "kudu/util/logging.h"
#include "kudu/util/mem_tracker.h"
#include "kudu/util/metrics.h"
#include "kudu/util/monotime.h"
#include "kudu/util/process_memory.h"
#include "kudu/util/status.h"
#include "kudu/util/string_case.h"
#include "kudu/util/web_callback_registry.h"

#ifdef TCMALLOC_ENABLED
#include "kudu/util/faststring.h"
#endif

using google::CommandLineFlagInfo;
using google::GetCommandLineFlagInfo;
using kudu::iequals;
using std::ifstream;
using std::ostringstream;
using std::shared_ptr;
using std::string;
using std::vector;
using strings::Substitute;

DEFINE_int64(web_log_bytes, 1024 * 1024,
    "The maximum number of bytes to display on the debug webserver's log page");
TAG_FLAG(web_log_bytes, advanced);
TAG_FLAG(web_log_bytes, runtime);

DEFINE_string(metrics_default_level, "debug",
              "The default severity level to use when filtering the metrics, "
              "both in JSON and Prometheus formats. Valid choices are 'debug', "
              "'info', and 'warn'. The levels are ordered and lower levels "
              "include the levels above them. This value can be overridden "
              "by passing the level query parameter to the '/metrics' endpoint "
              "when fetching metrics in JSON format.");
TAG_FLAG(metrics_default_level, advanced);
TAG_FLAG(metrics_default_level, runtime);
TAG_FLAG(metrics_default_level, evolving);
DEFINE_validator(metrics_default_level, [](const char* flag_name, const string& value) {
  if (iequals(value, "debug") ||
      iequals(value, "info") ||
      iequals(value, "warn")) {
    return true;
  }
  LOG(ERROR) << Substitute("unknown value for --$0 flag: '$1' "
                           "(expected one of 'debug', 'info', or 'warn')",
                           flag_name, value);
  return false;
});

// For configuration dashboard
DECLARE_bool(webserver_require_spnego);
DECLARE_string(redact);
DECLARE_string(rpc_encryption);
DECLARE_string(rpc_authentication);
DECLARE_string(webserver_certificate_file);

namespace kudu {

namespace {
// Html/Text formatting tags
struct Tags {
  string pre_tag, end_pre_tag, line_break, header, end_header;

  // If as_text is true, set the html tags to a corresponding raw text representation.
  explicit Tags(bool as_text) {
    if (as_text) {
      pre_tag = "";
      end_pre_tag = "\n";
      line_break = "\n";
      header = "";
      end_header = "";
    } else {
      pre_tag = "<pre>";
      end_pre_tag = "</pre>";
      line_break = "<br/>";
      header = "<h2>";
      end_header = "</h2>";
    }
  }
};
} // anonymous namespace

// Writes the last FLAGS_web_log_bytes of the INFO logfile to a webpage
// Note to get best performance, set GLOG_logbuflevel=-1 to prevent log buffering
static void LogsHandler(const Webserver::WebRequest& req, Webserver::WebResponse* resp) {
  EasyJson* output = &resp->output;
  (*output)["raw"] = (req.parsed_args.find("raw") != req.parsed_args.end());
  string logfile;
  GetFullLogFilename(google::INFO, &logfile);
  (*output)["logfile"] = logfile;
  struct stat file_stat;
  if (stat(logfile.c_str(), &file_stat) == 0) {
    size_t size = file_stat.st_size;
    size_t seekpos = size < FLAGS_web_log_bytes ? 0L : size - FLAGS_web_log_bytes;
    ifstream log(logfile.c_str(), std::ios::in);
    // Note if the file rolls between stat and seek, this could fail
    // (and we could wind up reading the whole file). But because the
    // file is likely to be small, this is unlikely to be an issue in
    // practice.
    log.seekg(seekpos);
    (*output)["web_log_bytes"] = FLAGS_web_log_bytes;
    ostringstream ss;
    ss << log.rdbuf();
    (*output)["log"] = ss.str();
  }
}

// Registered to handle "/flags", and prints out all command-line flags and their HTML
// escaped values. If --redact indicates that redaction is enabled for the web UI, the
// values of flags tagged as sensitive will be redacted. The values would not be HTML
// escaped if in the raw text mode, e.g. "/varz?raw".
static void FlagsHandler(const Webserver::WebRequest& req,
                         Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = &resp->output;
  bool as_text = (req.parsed_args.find("raw") != req.parsed_args.end());
  Tags tags(as_text);

  (*output) << tags.header << "Non-default Command-line Flags" << tags.end_header;
  (*output) << tags.pre_tag
            << CommandlineFlagsIntoString(as_text ? EscapeMode::NONE : EscapeMode::HTML,
                                          Selection::NONDEFAULT)
            << tags.end_pre_tag;

  (*output) << tags.header << "All Command-line Flags" << tags.end_header;
  (*output) << tags.pre_tag
            << CommandlineFlagsIntoString(as_text ? EscapeMode::NONE : EscapeMode::HTML,
                                          Selection::ALL)
            << tags.end_pre_tag;
}

// Registered to handle "/stacks".
//
// Prints out the current stack trace of all threads in the process.
static void StacksHandler(const Webserver::WebRequest& /*req*/,
                          Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = &resp->output;

  StackTraceSnapshot snap;
  auto start = MonoTime::Now();
  Status s = snap.SnapshotAllStacks();
  if (!s.ok()) {
    *output << "Failed to collect stacks: " << s.ToString();
    return;
  }
  auto dur = MonoTime::Now() - start;

  *output << "Collected stacks from " << snap.num_threads() << " threads in "
          << dur.ToString() << "\n";
  if (snap.num_failed()) {
    *output << "Failed to collect stacks from " << snap.num_failed() << " threads "
            << "(they may have exited while we were iterating over the threads)\n";
  }
  *output << "\n";
  snap.VisitGroups([&](ArrayView<StackTraceSnapshot::ThreadInfo> threads) {
      if (threads.size() > 1) {
        *output << threads.size() << " threads with same stack:\n";
      }

      for (auto& info : threads) {
        *output << "TID " << info.tid << "(" << info.thread_name << "):\n";
      }
      *output << threads[0].stack.Symbolize() << "\n\n";
    });
}

// Registered to handle "/version"
//
// Prints out the current version info
static void VersionInfoHandler(const Webserver::WebRequest& /*req*/,
                               Webserver::PrerenderedWebResponse* resp) {
  JsonWriter writer(&resp->output, JsonWriter::PRETTY);
  writer.StartObject();
  writer.String("version_info");

  kudu::VersionInfoPB version_info;
  VersionInfo::GetVersionInfoPB(&version_info);

  writer.Protobuf(version_info);
  writer.EndObject();
}

// Registered to handle "/memz", and prints out memory allocation statistics.
static void MemUsageHandler(const Webserver::WebRequest& req,
                            Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = &resp->output;
  bool as_text = (req.parsed_args.find("raw") != req.parsed_args.end());
  Tags tags(as_text);

  (*output) << tags.pre_tag;
#ifndef TCMALLOC_ENABLED
  (*output) << "Memory tracking is not available unless tcmalloc is enabled.";
#else
  faststring buf;
  buf.resize(32 * 1024);
  MallocExtension::instance()->GetStats(reinterpret_cast<char*>(buf.data()), buf.size());
  // Replace new lines with <br> for html
  string tmp(reinterpret_cast<char*>(buf.data()));
  boost::replace_all(tmp, "\n", tags.line_break);
  (*output) << tmp << tags.end_pre_tag;
#endif
}

// Registered to handle "/mem-trackers", and prints out memory tracker information.
static void MemTrackersHandler(const Webserver::WebRequest& /*req*/,
                               Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = &resp->output;
  int64_t current_consumption = process_memory::CurrentConsumption();
  int64_t hard_limit = process_memory::HardLimit();
  *output << "<h1>Process memory usage</h1>\n";
  *output << "<table class='table table-striped'>\n";
  *output << Substitute("  <tr><th>Total consumption</th><td>$0</td></tr>\n",
                        HumanReadableNumBytes::ToString(current_consumption));
  *output << Substitute("  <tr><th>Memory limit</th><td>$0</td></tr>\n",
                        HumanReadableNumBytes::ToString(hard_limit));
  if (hard_limit > 0) {
    double percentage = 100 * static_cast<double>(current_consumption) / hard_limit;
    *output << Substitute("  <tr><th>Percentage consumed</th><td>$0%</td></tr>\n",
                          StringPrintf("%.2f", percentage));
  }
  *output << "</table>\n";
#ifndef TCMALLOC_ENABLED
  *output << R"(
      <div class="alert alert-warning">
        <strong>NOTE:</strong> This build of Kudu has not enabled tcmalloc.
        The above process memory stats will be inaccurate.
      </div>
               )";
#endif

  *output << "<h1>Memory usage by subsystem</h1>\n";
  *output << "<table data-toggle='table' "
             "       data-pagination='true' "
             "       data-search='true' "
             "       class='table table-striped'>\n";
  *output << "<thead><tr>"
             "<th>Id</th>"
             "<th>Parent</th>"
             "<th>Limit</th>"
             "<th data-sorter='bytesSorter' "
             "    data-sortable='true' "
             ">Current Consumption</th>"
             "<th data-sorter='bytesSorter' "
             "    data-sortable='true' "
             ">Peak Consumption</th>";
  *output << "<tbody>\n";

  vector<shared_ptr<MemTracker> > trackers;
  MemTracker::ListTrackers(&trackers);
  for (const shared_ptr<MemTracker>& tracker : trackers) {
    string parent = tracker->parent() == nullptr ? "none" : tracker->parent()->id();
    string limit_str = tracker->limit() == -1 ? "none" :
                       HumanReadableNumBytes::ToString(tracker->limit());
    string current_consumption_str = HumanReadableNumBytes::ToString(tracker->consumption());
    string peak_consumption_str = HumanReadableNumBytes::ToString(tracker->peak_consumption());
    (*output) << Substitute("<tr><td>$0</td><td>$1</td><td>$2</td>" // id, parent, limit
                            "<td>$3</td><td>$4</td></tr>\n", // current, peak
                            tracker->id(), parent, limit_str, current_consumption_str,
                            peak_consumption_str);
  }
  *output << "</tbody></table>\n";
}

static void HealthHandler(const Webserver::WebRequest& /*req*/,
                          Webserver::PrerenderedWebResponse* resp) {
  resp->output << "OK";
  resp->status_code = HttpStatusCode::Ok;
}

static const char* const kName = "name";
static const char* const kValue = "value";
static const char* const kId = "id";
static const char* const kComment = "comment";
static const char* const kSecure = "secure";

static void FillSecurityConfigs(EasyJson* output) {
  EasyJson configs = output->Set("security_configs", EasyJson::kArray);

  EasyJson rpc_encryption = configs.PushBack(EasyJson::kObject);
  rpc_encryption[kName] = "RPC Encryption";
  rpc_encryption[kValue] = FLAGS_rpc_encryption;
  rpc_encryption[kSecure] = iequals(FLAGS_rpc_encryption, "required");
  rpc_encryption[kId] = "rpc_encryption";
  rpc_encryption[kComment] =
      "Configure with --rpc_encryption. Most secure value is 'required'.";

  EasyJson rpc_authentication = configs.PushBack(EasyJson::kObject);
  rpc_authentication[kName] = "RPC Authentication";
  rpc_authentication[kValue] = FLAGS_rpc_authentication;
  rpc_authentication[kSecure] = iequals(FLAGS_rpc_authentication, "required");
  rpc_authentication[kId] = "rpc_authentication";
  rpc_authentication[kComment] =
      "Configure with --rpc_authentication. Most secure value is 'required'.";

  EasyJson webserver_encryption = configs.PushBack(EasyJson::kObject);
  webserver_encryption[kName] = "Webserver Encryption";
  webserver_encryption[kValue] = FLAGS_webserver_certificate_file.empty() ? "off" : "on";
  webserver_encryption[kSecure] = !FLAGS_webserver_certificate_file.empty();
  webserver_encryption[kId] = "webserver_encryption";
  webserver_encryption[kComment] =
      "Configure with --webserver_certificate_file and --webserver_private_key_file.";

  EasyJson webserver_redaction = configs.PushBack(EasyJson::kObject);
  webserver_redaction[kName] = "Webserver Redaction";
  webserver_redaction[kValue] = FLAGS_redact;
  webserver_redaction[kSecure] = iequals(FLAGS_redact, "all");
  webserver_redaction[kId] = "webserver_redaction";
  webserver_redaction[kComment] =
      "Configure with --redact. Most secure value is 'all'.";

  EasyJson webserver_spnego = configs.PushBack(EasyJson::kObject);
  webserver_spnego[kName] = "Webserver Kerberos Authentication via SPNEGO";
  webserver_spnego[kValue] = FLAGS_webserver_require_spnego ? "on" : "off";
  webserver_spnego[kSecure] = FLAGS_webserver_require_spnego;
  webserver_spnego[kId] = "webserver_spnego";
  webserver_spnego[kComment] = "Configure with --webserver_require_spnego.";
}

// Information on the configured and the effective time source for a server.
static void FillTimeSourceConfigs(EasyJson* output) {
  CommandLineFlagInfo flag_info;
  auto rc = GetCommandLineFlagInfo("time_source", &flag_info);
  CHECK(rc) << "could not get information on 'time_source' flag";

  EasyJson configs = output->Set("time_source_configs", EasyJson::kArray);
  EasyJson time_source_configured = configs.PushBack(EasyJson::kObject);
  time_source_configured[kName] = "Configured Time Source";
  time_source_configured[kValue] = flag_info.current_value;
  time_source_configured[kId] = "time_source_configured";
  time_source_configured[kComment] =
      "Time source for HybridClock timestamps generated by Kudu masters and "
      "tablet servers. Configurable via the --time_source flag.";

  // In case if the time source configured as 'auto', the default value of the
  // flag is updated to reflect the auto-selected/effective one. In all other
  // cases, the effective time source is the same as the configured one.
  const bool is_auto_source = (flag_info.current_value == "auto");
  const string time_source = is_auto_source ? flag_info.default_value
                                            : flag_info.current_value;
  EasyJson time_source_effective = configs.PushBack(EasyJson::kObject);
  time_source_effective[kName] = "Effective Time Source";
  time_source_effective[kValue] = time_source;
  time_source_effective[kId] = "time_source_effective";
  time_source_effective[kComment] =
      "Effective Time Source: the system auto-selects the best option "
      "depending on the hosting environment when configured with "
      "--time_source=auto. Otherwise, the Effective Time Source is the same "
      "as the Configured Time Source.";

  // In case if the effective time source is 'builtin', provide information
  // on the configured NTP servers.
  if (time_source == "builtin") {
    CommandLineFlagInfo flag_info;
    auto rc = GetCommandLineFlagInfo("builtin_ntp_servers", &flag_info);
    CHECK(rc) << "could not get information on 'builtin_ntp_servers' flag";
    const string ntp_servers = is_auto_source ? flag_info.default_value
                                              : flag_info.current_value;
    EasyJson builtin_ntp_servers = configs.PushBack(EasyJson::kObject);
    builtin_ntp_servers[kName] = "NTP Servers for Built-in NTP Client";
    builtin_ntp_servers[kValue] = ntp_servers;
    builtin_ntp_servers[kId] = "builtin_ntp_servers";
    builtin_ntp_servers[kComment] =
        "Effective list of NTP servers used by the built-in NTP client. "
        "Configurable via --builtin_ntp_servers. If Kudu is configured with "
        "--time_source=auto and the Effective Time Source is auto-selected "
        "to be 'builtin', Kudu tries to use dedicated NTP servers provided by "
        "the hosting environment known to Kudu, overriding the list of servers "
        "configured via --builtin_ntp_servers. If Kudu cannot recognize the "
        "hosting environment it runs with --time_source=auto, the Effective "
        "Time Source is auto-selected to be 'builtin' with the set of "
        "reference servers configured per --builtin_ntp_servers flag's value, "
        "unless it's empty or otherwise unparsable. The last resort for a "
        "cluster-wide synchronized clock is to auto-select the 'system' Time "
        "Source if the platform supports get_ntptime() API. The catch-all case "
        "is 'system_unsync' Time Source which is for development-only "
        "platforms or single-node-runs-it-all proof-of-concept Kudu clusters.";
  }
}

static void ConfigurationHandler(const Webserver::WebRequest& /* req */,
                                 Webserver::WebResponse* resp) {
  EasyJson* output = &resp->output;
  FillSecurityConfigs(output);
  FillTimeSourceConfigs(output);
}
void AddPreInitializedDefaultPathHandlers(Webserver* webserver) {
  bool on_nav_bar = true;
  webserver->RegisterPathHandler("/logs", "Logs", LogsHandler, StyleMode::STYLED, on_nav_bar);
  webserver->RegisterPrerenderedPathHandler("/varz", "Flags", FlagsHandler, StyleMode::STYLED,
                                            on_nav_bar);
  webserver->RegisterPathHandler("/config", "Configuration", ConfigurationHandler,
                                 StyleMode::STYLED, on_nav_bar);
  webserver->RegisterPrerenderedPathHandler("/memz", "Memory (total)", MemUsageHandler,
                                            StyleMode::STYLED, on_nav_bar);
  webserver->RegisterPrerenderedPathHandler("/mem-trackers", "Memory (detail)", MemTrackersHandler,
                                            StyleMode::STYLED, on_nav_bar);
}

void AddPostInitializedDefaultPathHandlers(Webserver* webserver) {
  webserver->RegisterPrerenderedPathHandler("/stacks", "Stacks", StacksHandler,
                                            StyleMode::UNSTYLED,
                                            /*is_on_nav_bar=*/true);
  webserver->RegisterPrerenderedPathHandler("/version", "VersionInfo", VersionInfoHandler,
                                            StyleMode::UNSTYLED,
                                            /*is_on_nav_bar*/false);
  webserver->RegisterPrerenderedPathHandler("/healthz", "Health", HealthHandler,
                                            StyleMode::UNSTYLED,
                                            /*is_on_nav_bar=*/true);
  AddPprofPathHandlers(webserver);
}

static bool ParseBool(const Webserver::ArgumentMap& args, const string& key) {
  static const string kDefaultValue = "false";
  const auto& arg = FindWithDefault(args, key, kDefaultValue);
  return ParseLeadingBoolValue(arg.c_str(), false);
}

static vector<string> ParseArray(const Webserver::ArgumentMap& args, const string& key) {
  vector<string> value;
  const string* arg = FindOrNull(args, key);
  if (arg != nullptr) {
    SplitStringUsing(*arg, ",", &value);
  }
  return value;
}

static void WriteMetricsAsJson(const MetricRegistry* const metrics,
                               const Webserver::WebRequest& req,
                               Webserver::PrerenderedWebResponse* resp) {
  MetricJsonOptions opts;
  opts.include_raw_histograms = ParseBool(req.parsed_args, "include_raw_histograms");
  opts.include_schema_info = ParseBool(req.parsed_args, "include_schema");

  MetricFilters& filters = opts.filters;
  filters.entity_types = ParseArray(req.parsed_args, "types");
  filters.entity_ids = ParseArray(req.parsed_args, "ids");
  filters.entity_attrs = ParseArray(req.parsed_args, "attributes");
  filters.entity_metrics = ParseArray(req.parsed_args, "metrics");
  filters.entity_level = FindWithDefault(req.parsed_args, "level", FLAGS_metrics_default_level);
  vector<string> merge_rules = ParseArray(req.parsed_args, "merge_rules");
  for (const auto& merge_rule : merge_rules) {
    vector<string> values;
    SplitStringUsing(merge_rule, "|", &values);
    if (values.size() == 3) {
      // Index 0: entity type needed to be merged.
      // Index 1: 'merge_to' field of MergeAttributes.
      // Index 2: 'attribute_to_merge_by' field of MergeAttributes.
      EmplaceIfNotPresent(&opts.merge_rules, values[0], MergeAttributes(values[1], values[2]));
    }
  }

  JsonWriter::Mode json_mode = ParseBool(req.parsed_args, "compact") ?
      JsonWriter::COMPACT : JsonWriter::PRETTY;

  // The number of entity_attrs should always be even because
  // each pair represents a key and a value.
  if (filters.entity_attrs.size() % 2 != 0) {
    resp->status_code = HttpStatusCode::BadRequest;
    WARN_NOT_OK(Status::InvalidArgument(""), "The parameter of 'attributes' is wrong");
  } else {
    JsonWriter writer(&resp->output, json_mode);
    WARN_NOT_OK(metrics->WriteAsJson(&writer, opts), "Couldn't write JSON metrics over HTTP");
  }
}

static void WriteMetricsAsPrometheus(const MetricRegistry* const metrics,
                                     const Webserver::WebRequest& /*req*/,
                                     Webserver::PrerenderedWebResponse* resp) {
  MetricPrometheusOptions opts;
  opts.filters.entity_level = FLAGS_metrics_default_level;

  PrometheusWriter writer(&resp->output);
  WARN_NOT_OK(metrics->WriteAsPrometheus(&writer, opts),
              "couldn't write Prometheus metrics over HTTP");
}

void RegisterMetricsJsonHandler(Webserver* webserver, const MetricRegistry* const metrics) {
  auto callback = [metrics](const Webserver::WebRequest& req,
                            Webserver::PrerenderedWebResponse* resp) {
    WriteMetricsAsJson(metrics, req, resp);
  };
  bool not_on_nav_bar = false;
  bool is_on_nav_bar = true;
  webserver->RegisterJsonPathHandler("/metrics", "JSON Metrics", callback, is_on_nav_bar);

  // The old name -- this is preserved for compatibility with older releases of
  // monitoring software which expects the old name.
  webserver->RegisterJsonPathHandler("/jsonmetricz", "Metrics", callback, not_on_nav_bar);
}

void RegisterMetricsPrometheusHandler(Webserver* webserver, const MetricRegistry* const metrics) {
  auto callback = [metrics](const Webserver::WebRequest& req,
                            Webserver::PrerenderedWebResponse* resp) {
    WriteMetricsAsPrometheus(metrics, req, resp);
  };
  constexpr bool is_on_nav_bar = true;
  webserver->RegisterPrerenderedPathHandler("/metrics_prometheus", "Prometheus Metrics", callback,
                                            StyleMode::UNSTYLED, is_on_nav_bar);
}

} // namespace kudu
