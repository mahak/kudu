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

#include "kudu/util/test_util.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <fcntl.h>
#include <sys/param.h> // for MAXPATHLEN
#endif

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest-spi.h>

#include "kudu/gutil/strings/escaping.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/gutil/strings/split.h"
#include "kudu/gutil/strings/strcat.h"
#include "kudu/gutil/strings/stringpiece.h"
#include "kudu/gutil/strings/strip.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/gutil/walltime.h"
#include "kudu/util/env.h"
#include "kudu/util/faststring.h"
#include "kudu/util/flags.h"
#include "kudu/util/oid_generator.h"
#include "kudu/util/path_util.h"
#include "kudu/util/scoped_cleanup.h"
#include "kudu/util/slice.h"
#include "kudu/util/spinlock_profiling.h"
#include "kudu/util/status.h"
#include "kudu/util/string_case.h"
#include "kudu/util/subprocess.h"
#include "kudu/util/test_macros.h"

DEFINE_string(test_leave_files, "on_failure",
              "Whether to leave test files around after the test run. "
              "Valid values are 'always', 'on_failure', or 'never'");

DEFINE_int32(test_random_seed, 0, "Random seed to use for randomized tests");

DECLARE_string(time_source);
DECLARE_bool(enable_multi_tenancy);
DECLARE_bool(encrypt_data_at_rest);

using std::string;
using std::unordered_map;
using std::vector;
using strings::Substitute;

namespace {
int testIteration = 0;
} // namespace

namespace kudu {

const char* kInvalidPath = "/dev/invalid-path-for-kudu-tests";
static const char* const kSlowTestsEnvVar = "KUDU_ALLOW_SLOW_TESTS";
static const char* const kLargeKeysEnvVar = "KUDU_USE_LARGE_KEYS_IN_TESTS";
static const char* const kEncryptDataInTests = "KUDU_ENCRYPT_DATA_IN_TESTS";
static const int kEncryptionKeySize = 16;
static const uint8_t kEncryptionKey[kEncryptionKeySize] =
  {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 42};
static const uint8_t kEncryptionKeyIv[kEncryptionKeySize] =
  {42, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
static const char* const kEncryptionKeyVersion = "kudutenantkey@0";
static const char* const kEncryptionTenantName = "default_tenant_kudu";
static const char* const kEncryptionTenantID = "00000000000000000000000000000000";

static const uint64_t kTestBeganAtMicros = Env::Default()->NowMicros();

static const char* const kContentTypeTextPlain = "text/plain";
static const char* const kContentTypeTextHtml = "text/html";
static const char* const kContentTypeApplicationOctet = "application/octet-stream";
static const char* const kContentTypeApplicationJson = "application/json";

// Global which production code can check to see if it is running
// in a GTest environment (assuming the test binary links in this module,
// which is typically a good assumption).
//
// This can be checked using the 'IsGTest()' function from test_util_prod.cc.
bool g_is_gtest = true;

void KuduTestEventListener::OnTestIterationStart(const testing::UnitTest& /*unit_test*/,
                                                 int iteration) {
  testIteration = iteration;
}

///////////////////////////////////////////////////
// KuduTest
///////////////////////////////////////////////////

KuduTest::KuduTest()
    : env_(Env::Default()),
      flag_saver_(new google::FlagSaver()),
      test_dir_(GetTestDataDirectory()) {
  std::map<const char*, const char*> flags_for_tests = {
    // Disabling fsync() speeds up tests dramatically, and it's safe to do as no
    // tests rely on cutting power to a machine or equivalent.
    {"never_fsync", "true"},
    // Disable redaction.
    {"redact", "none"},
    // For a generic Kudu test, the local wall-clock time is good enough even
    // if it's not synchronized by NTP. All test components are run at the same
    // node, so there aren't multiple time sources to synchronize.
    {"time_source", "system_unsync"},
  };
  if (!UseLargeKeys()) {
    // Reduce default RSA key length for faster tests. We are using strong/high
    // TLS v1.2 cipher suites, so minimum possible for TLS-related RSA keys is
    // 768 bits. Java security policies in tests tweaked appropriately to allow
    // for using smaller RSA keys in certificates. As for the TSK keys, 512 bits
    // is the minimum since the SHA256 digest is used for token
    // signing/verification.
    flags_for_tests.emplace("ipki_server_key_size", "768");
    flags_for_tests.emplace("ipki_ca_key_size", "768");
    flags_for_tests.emplace("tsk_num_rsa_bits", "512");
    // Some OS distros set the default security level higher than 0, so it's
    // necessary to override it to use the key length specified above (which are
    // considered lax and don't work in case of security level 2 or higher).
    flags_for_tests.emplace("openssl_security_level_override", "0");
  }
  for (const auto& e : flags_for_tests) {
    // We don't check for errors here, because we have some default flags that
    // only apply to certain tests. If a flag is defined in a library which
    // the test binary isn't linked with, then SetCommandLineOptionWithMode()
    // reports an error since the flag is unknown to the gflags runtime.
    google::SetCommandLineOptionWithMode(e.first, e.second, google::SET_FLAGS_DEFAULT);
  }

  if (EnableEncryption()) {
    SetEncryptionFlags(true);
  }

  // If the TEST_TMPDIR variable has been set, then glog will automatically use that
  // as its default log directory. We would prefer that the default log directory
  // instead be the test-case-specific subdirectory.
  FLAGS_log_dir = test_dir_;
}

KuduTest::~KuduTest() {
  // Reset the flags first to prevent them from affecting test directory cleanup.
  flag_saver_.reset();

  // Clean up the test directory in the destructor instead of a TearDown
  // method. This is better because it ensures that the child-class
  // dtor runs first -- so, if the child class is using a minicluster, etc,
  // we will shut that down before we remove files underneath.
  if (FLAGS_test_leave_files == "always") {
    LOG(INFO) << "-----------------------------------------------";
    LOG(INFO) << "--test_leave_files specified, leaving files in " << test_dir_;
  } else if (FLAGS_test_leave_files == "on_failure" && HasFailure()) {
    LOG(INFO) << "-----------------------------------------------";
    LOG(INFO) << "Had failures, leaving test files at " << test_dir_;
  } else {
    VLOG(1) << "Cleaning up temporary test files...";
    WARN_NOT_OK(env_->DeleteRecursively(test_dir_),
                "Couldn't remove test files");
  }
}

void KuduTest::SetUp() {
  InitSpinLockContentionProfiling();
  OverrideKrb5Environment();
}

string KuduTest::GetTestPath(const string& relative_path) const {
  return JoinPathSegments(test_dir_, relative_path);
}

void KuduTest::OverrideKrb5Environment() {
  // Set these variables to paths that definitely do not exist and
  // couldn't be accidentally created.
  //
  // Note that if we were to set these to /dev/null, we end up triggering a leak in krb5
  // when it tries to read an empty file as a ticket cache, whereas non-existent files
  // don't have this issue. See MIT krb5 bug #8509.
  //
  // NOTE: we don't simply *unset* the variables, because then we'd still pick up
  // the user's /etc/krb5.conf and other default locations.
  PCHECK(setenv("KRB5_CONFIG", kInvalidPath, 1) == 0);
  PCHECK(setenv("KRB5_KTNAME", kInvalidPath, 1) == 0);
  PCHECK(setenv("KRB5CCNAME", kInvalidPath, 1) == 0);
}

void KuduTest::SetEncryptionFlags(bool enable_encryption) {
  FLAGS_encrypt_data_at_rest = enable_encryption;
  if (enable_encryption) {
    Env::Default()->SetEncryptionKey(kEncryptionKey, kEncryptionKeySize * 8);
  }
}

void KuduTest::GetEncryptionKey(string* name, string* id, string* key, string* iv,
                                string* version) {
  if (FLAGS_encrypt_data_at_rest) {
    if (FLAGS_enable_multi_tenancy && name && id) {
      *name = kEncryptionTenantName;
      *id = kEncryptionTenantID;
    }
    strings::b2a_hex(kEncryptionKey, key, kEncryptionKeySize);
    strings::b2a_hex(kEncryptionKeyIv, iv, kEncryptionKeySize);
    *version = kEncryptionKeyVersion;
  } else {
    if (name && id) {
      *name = "";
      *id = "";
    }
    *key = "";
    *iv = "";
    *version = "";
  }
}

///////////////////////////////////////////////////
// Test utility functions
///////////////////////////////////////////////////

bool AllowSlowTests() { return GetBooleanEnvironmentVariable(kSlowTestsEnvVar); }

bool UseLargeKeys() { return GetBooleanEnvironmentVariable(kLargeKeysEnvVar); }

bool EnableEncryption() { return GetBooleanEnvironmentVariable(kEncryptDataInTests); }

void OverrideFlagForSlowTests(const std::string& flag_name,
                              const std::string& new_value) {
  // Ensure that the flag is valid.
  google::GetCommandLineFlagInfoOrDie(flag_name.c_str());

  // If we're not running slow tests, don't override it.
  if (!AllowSlowTests()) {
    return;
  }
  google::SetCommandLineOptionWithMode(flag_name.c_str(), new_value.c_str(),
                                       google::SET_FLAG_IF_DEFAULT);
}

int SeedRandom() {
  int seed;
  // Initialize random seed
  if (FLAGS_test_random_seed == 0) {
    // Not specified by user
    seed = static_cast<int>(GetCurrentTimeMicros());
  } else {
    seed = FLAGS_test_random_seed;
  }
  LOG(INFO) << "Using random seed: " << seed;
  srand(seed);
  return seed;
}

string GetTestDataDirectory() {
  const ::testing::TestInfo* const test_info =
    ::testing::UnitTest::GetInstance()->current_test_info();
  CHECK(test_info) << "Must be running in a gtest unit test to call this function";
  string dir;
  CHECK_OK(Env::Default()->GetTestDirectory(&dir));

  // The directory name includes some strings for specific reasons:
  // - program name: identifies the directory to the test invoker
  // - timestamp and pid: disambiguates with prior runs of the same test
  // - iteration: identifies the iteration when using --gtest_repeat
  //
  // e.g. "env-test.TestEnv.TestReadFully.1409169025392361-23600-0"
  //
  // If the test is sharded, the shard index is also included so that the test
  // invoker can more easily identify all directories belonging to each shard.
  string shard_index_infix;
  const char* shard_index = getenv("GTEST_SHARD_INDEX");
  if (shard_index && shard_index[0] != '\0') {
    shard_index_infix = Substitute("$0.", shard_index);
  }
  dir += Substitute("/$0.$1$2.$3.$4-$5-$6",
    StringReplace(google::ProgramInvocationShortName(), "/", "_", true),
    shard_index_infix,
    StringReplace(test_info->test_case_name(), "/", "_", true),
    StringReplace(test_info->name(), "/", "_", true),
    kTestBeganAtMicros,
    getpid(),
    testIteration);
  Status s = Env::Default()->CreateDir(dir);
  CHECK(s.IsAlreadyPresent() || s.ok())
    << "Could not create directory " << dir << ": " << s.ToString();
  if (s.ok()) {
    string metadata;

    StrAppend(&metadata, Substitute("PID=$0\n", getpid()));

    StrAppend(&metadata, Substitute("PPID=$0\n", getppid()));

    char* jenkins_build_id = getenv("BUILD_ID");
    if (jenkins_build_id) {
      StrAppend(&metadata, Substitute("BUILD_ID=$0\n", jenkins_build_id));
    }

    CHECK_OK(WriteStringToFile(Env::Default(), metadata,
                               Substitute("$0/test_metadata", dir)));
  }
  return dir;
}

string GetTestSocketPath(const string& name) {
  string dir;
  CHECK_OK(Env::Default()->GetTestDirectory(&dir));
  ObjectIdGenerator generator;
  string uuid = generator.Next();
  return JoinPathSegments(dir, Substitute("$0-$1.sock", name, uuid));
}

string GetTestExecutableDirectory() {
  string exec;
  CHECK_OK(Env::Default()->GetExecutablePath(&exec));
  return DirName(exec);
}

void AssertEventually(const std::function<void(void)>& f,
                      const MonoDelta& timeout,
                      AssertBackoff backoff) {
  const MonoTime deadline = MonoTime::Now() + timeout;
  {
    // Disable gtest's "on failure" behavior, or else the assertion failures
    // inside our attempts will cause the test to end even though we would
    // like to retry.
    bool old_break_on_failure = testing::FLAGS_gtest_break_on_failure;
    bool old_throw_on_failure = testing::FLAGS_gtest_throw_on_failure;
    auto c = MakeScopedCleanup([old_break_on_failure, old_throw_on_failure]() {
      testing::FLAGS_gtest_break_on_failure = old_break_on_failure;
      testing::FLAGS_gtest_throw_on_failure = old_throw_on_failure;
    });
    testing::FLAGS_gtest_break_on_failure = false;
    testing::FLAGS_gtest_throw_on_failure = false;

    for (int attempts = 0; MonoTime::Now() < deadline; attempts++) {
      // Capture any assertion failures within this scope (i.e. from their function)
      // into 'results'
      testing::TestPartResultArray results;
      testing::ScopedFakeTestPartResultReporter reporter(
          testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD,
          &results);
      f();

      // Determine whether their function produced any new test failure results.
      bool has_failures = false;
      for (int i = 0; i < results.size(); i++) {
        has_failures |= results.GetTestPartResult(i).failed();
      }
      if (!has_failures) {
        return;
      }

      // If they had failures, sleep and try again.
      int sleep_ms;
      switch (backoff) {
        case AssertBackoff::EXPONENTIAL:
          sleep_ms = (attempts < 10) ? (1 << attempts) : 1000;
          break;
        case AssertBackoff::NONE:
          sleep_ms = 1;
          break;
        default:
          LOG(FATAL) << "Unknown backoff type";
      }
      SleepFor(MonoDelta::FromMilliseconds(sleep_ms));
    }
  }

  // If we ran out of time looping, run their function one more time
  // without capturing its assertions. This way the assertions will
  // propagate back out to the normal test reporter. Of course it's
  // possible that it will pass on this last attempt, but that's OK
  // too, since we aren't trying to be that strict about the deadline.
  f();
  if (testing::Test::HasFatalFailure()) {
    ADD_FAILURE() << "Timed out waiting for assertion to pass.";
  }
}

int CountOpenFds(Env* env, const string& path_pattern) {
  static const char* kProcSelfFd =
#if defined(__APPLE__)
    "/dev/fd";
#else
    "/proc/self/fd";
#endif // defined(__APPLE__)
  faststring path_buf;
  vector<string> children;
  CHECK_OK(env->GetChildren(kProcSelfFd, &children));
  int num_fds = 0;
  for (const auto& c : children) {
    // Skip '.' and '..'.
    if (c == "." || c == "..") {
      continue;
    }
    int32_t fd;
    CHECK(safe_strto32(c, &fd)) << "Unexpected file in fd list: " << c;
#ifdef __APPLE__
    path_buf.resize(MAXPATHLEN);
    if (fcntl(fd, F_GETPATH, path_buf.data()) != 0) {
      if (errno == EBADF) {
        // The file was closed while we were looping. This is likely the
        // actual file descriptor used for opening /proc/fd itself.
        continue;
      }
      PLOG(FATAL) << "Unknown error in fcntl(F_GETPATH): " << fd;
    }
    char* buf_data = reinterpret_cast<char*>(path_buf.data());
    path_buf.resize(strlen(buf_data));
#else
    path_buf.resize(PATH_MAX);
    char* buf_data = reinterpret_cast<char*>(path_buf.data());
    auto proc_file = JoinPathSegments(kProcSelfFd, c);
    int path_len = readlink(proc_file.c_str(), buf_data, path_buf.size());
    if (path_len < 0) {
      if (errno == ENOENT) {
        // The file was closed while we were looping. This is likely the
        // actual file descriptor used for opening /proc/fd itself.
        continue;
      }
      PLOG(FATAL) << "Unknown error in readlink: " << proc_file;
    }
    path_buf.resize(path_len);
#endif
    if (!MatchPattern(path_buf.ToString(), path_pattern)) {
      continue;
    }
    num_fds++;
  }

  return num_fds;
}

namespace {
const vector<string> kWildcard = { "0.0.0.0" };

Status WaitForBind(pid_t pid,
                   uint16_t* port,
                   const vector<string>& addresses,
                   const char* kind,
                   MonoDelta timeout) {
  // In general, processes do not expose the port they bind to, and
  // reimplementing lsof involves parsing a lot of files in /proc/. So,
  // requiring lsof for tests and parsing its output seems more
  // straight-forward. We call lsof in a loop since it typically takes a long
  // time for it to initialize and bind a port.

  string lsof;
  RETURN_NOT_OK(FindExecutable("lsof", {"/sbin", "/usr/sbin"}, &lsof));

  const vector<string> cmd = {
    lsof, "-wnP", "-Ffn",
    "-p", std::to_string(pid),
    "-a", "-i", kind
  };

  // The '-Ffn' flag gets lsof to output something like:
  //   p5801
  //   f548
  //   n127.0.0.1:43954->127.0.0.1:43617
  //   f549
  //   n*:8038
  //
  // The first line is the pid. We ignore it.
  // Subsequent lines come in pairs. In each pair, the first half of the pair
  // is file descriptor number, we ignore it.
  // The second half has the bind address and port.
  //
  // In this example, the first pair is an outbound TCP socket. We ignore it.
  // The second pair is the listening TCP socket bind address and port.
  //
  // We use the first encountered listening TCP socket, since that's most likely
  // to be the primary service port. When searching, we use the provided bind
  // address if there is any, otherwise we use '*' (same as '0.0.0.0') which
  // matches all addresses on the local machine.
  const MonoTime deadline = MonoTime::Now() + timeout;
  const auto& addresses_to_check = addresses.empty() ? kWildcard : addresses;
  for (int64_t i = 1; ; ++i) {
    for (const auto& addr : addresses_to_check) {
      const string addr_pattern = Substitute("n$0:", addr == "0.0.0.0" ? "*" : addr);
      string lsof_out;
      int32_t p = -1;
      const auto s = Subprocess::Call(cmd, "", &lsof_out).AndThen([&] () {
        StripTrailingNewline(&lsof_out);
        vector<string> lines = strings::Split(lsof_out, "\n");
        for (int index = 2; index < lines.size(); index += 2) {
          StringPiece cur_line(lines[index]);
          if (HasPrefixString(cur_line.ToString(), addr_pattern) &&
              !cur_line.contains("->")) {
            cur_line.remove_prefix(addr_pattern.size());
            if (!safe_strto32(cur_line.data(), cur_line.size(), &p)) {
              return Status::RuntimeError(Substitute(
                  "could not parse port number in string '$0' from lsof output",
                  string(cur_line.data(), cur_line.size())), lsof_out);
            }

            return Status::OK();
          }
        }

        return Status::NotFound(
            "could not find pattern of a bound port in lsof output", lsof_out);
      });

      if (s.ok()) {
        CHECK(p > 0 && p <= std::numeric_limits<uint16_t>::max())
            << "parsed invalid port: " << p;
        VLOG(1) << "Determined bound port: " << p;
        *port = static_cast<uint16_t>(p);

        return Status::OK();
      }
      if (deadline < MonoTime::Now()) {
        return Status::TimedOut(Substitute(
            "process with PID $0 is not yet bound to any port at the specified "
            "addresses; last attempt running lsof returned '$1'",
            pid, s.ToString()));
      }
    }
    auto time_left_ms = std::max<int64_t>(
        (deadline - MonoTime::Now()).ToMilliseconds(), 0);
    SleepFor(MonoDelta::FromMilliseconds(std::min<int64_t>(i * 10, time_left_ms)));
  }

  // Should not reach here.
  LOG(FATAL) << "could not determine bound port the process";
  __builtin_unreachable();
}

Status WaitForBindAtPort(const vector<string>& addresses,
                         uint16_t port,
                         const char* kind,
                         MonoDelta timeout) {
  string lsof;
  RETURN_NOT_OK(FindExecutable("lsof", {"/sbin", "/usr/sbin"}, &lsof));
  const vector<string> cmd = { lsof, "-wnP", "-Fpfn", "-a", "-i", kind };

  // The '-Fpfn' flag gets lsof to output something like:
  //   p2133
  //   f549
  //   n*:8038
  //   f550
  //   n*:8088
  //   p5801
  //   f548
  //   n127.0.0.1:43954->127.0.0.1:43617
  //   p95857
  //   f3
  //   n127.0.0.1:63337
  //
  // The first line is the pid for every process of the user. We ignore it.
  // Subsequent lines come in pairs. In each pair, the first half of the pair
  // is file descriptor number, we ignore it.
  // The second half has the bind address and port.
  const MonoTime deadline = MonoTime::Now() + timeout;
  const auto& addresses_to_check = addresses.empty() ? kWildcard : addresses;
  for (int64_t i = 1; ; ++i) {
    string lsof_out;
    RETURN_NOT_OK(Subprocess::Call(cmd, "", &lsof_out));
    StripTrailingNewline(&lsof_out);
    const vector<string> lines = strings::Split(lsof_out, "\n");

    for (const auto& addr : addresses_to_check) {
      const string addr_pattern = Substitute(
          "n$0:$1", addr == "0.0.0.0" ? "*" : addr, port);
      for (const auto& l : lines) {
        if (l.empty()) {
          return Status::RuntimeError("empty line in lsof output", lsof_out);
        }
        if (l[0] == 'p' || l[0] == 'f') {
          continue;
        }
        if (l[0] == 'n') {
          if (l == addr_pattern) {
            return Status::OK();
          }
          continue;
        }
        return Status::RuntimeError("unexpected lsof output", lsof_out);
      }
    }

    if (deadline < MonoTime::Now()) {
      break;
    }
    auto time_left_ms = std::max<int64_t>(
        (deadline - MonoTime::Now()).ToMilliseconds(), 0);
    SleepFor(MonoDelta::FromMilliseconds(std::min<int64_t>(i * 10, time_left_ms)));
  }

  return Status::TimedOut(
      Substitute("time out waiting for port $0 to be bound", port));
}

} // anonymous namespace

Status WaitForTcpBind(pid_t pid, uint16_t* port,
                      const vector<string>& addresses,
                      MonoDelta timeout) {
  return WaitForBind(pid, port, addresses, "4TCP", timeout);
}

Status WaitForUdpBind(pid_t pid, uint16_t* port,
                      const vector<string>& addresses,
                      MonoDelta timeout) {
  return WaitForBind(pid, port, addresses, "4UDP", timeout);
}

Status WaitForTcpBindAtPort(const vector<string>& addresses,
                            uint16_t port,
                            MonoDelta timeout) {
  return WaitForBindAtPort(addresses, port, "4TCP", timeout);
}

Status WaitForUdpBindAtPort(const vector<string>& addresses,
                            uint16_t port,
                            MonoDelta timeout) {
  return WaitForBindAtPort(addresses, port, "4UDP", timeout);
}

Status FindHomeDir(const string& name, const string& bin_dir, string* home_dir) {
  string name_upper;
  ToUpperCase(name, &name_upper);

  string env_var = Substitute("$0_HOME", name_upper);
  const char* env = std::getenv(env_var.c_str());
  string dir = env == nullptr ? JoinPathSegments(bin_dir, Substitute("$0-home", name)) : env;

  if (!Env::Default()->FileExists(dir)) {
    return Status::NotFound(Substitute("$0 directory does not exist", env_var), dir);
  }
  *home_dir = dir;
  return Status::OK();
}

const unordered_map<string, string>& GetCommonWebserverEndpoints() {
  static const unordered_map<string, string> common_endpoints = {
      {"logs", kContentTypeTextHtml},
      {"varz", kContentTypeTextHtml},
      {"config", kContentTypeTextHtml},
      {"memz", kContentTypeTextHtml},
      {"mem-trackers", kContentTypeTextHtml},
      {"stacks", kContentTypeTextPlain},
      {"version", kContentTypeTextPlain},
      {"healthz", kContentTypeTextPlain},
      {"metrics", kContentTypeApplicationJson},
      {"jsonmetricz", kContentTypeApplicationJson},
      {"metrics_prometheus", kContentTypeTextPlain},
      {"rpcz", kContentTypeApplicationJson},
      {"startup", kContentTypeTextHtml},
      {"pprof/cmdline", kContentTypeTextPlain},
      {"pprof/heap", kContentTypeTextPlain},
      {"pprof/growth", kContentTypeTextPlain},
      {"pprof/profile", kContentTypeTextPlain},
      {"pprof/symbol", kContentTypeTextPlain},
      {"pprof/contention", kContentTypeTextPlain},
      {"tracing/json/begin_monitoring", kContentTypeApplicationJson},
      {"tracing/json/end_monitoring", kContentTypeApplicationJson},
      {"tracing/json/capture_monitoring", kContentTypeApplicationJson},
      {"tracing/json/get_monitoring_status", kContentTypeApplicationJson},
      {"tracing/json/categories", kContentTypeApplicationJson},
      {"tracing/json/begin_recording", kContentTypeApplicationJson},
      {"tracing/json/get_buffer_percent_full", kContentTypeApplicationJson},
      {"tracing/json/end_recording", kContentTypeApplicationJson},
      {"tracing/json/end_recording_compressed", kContentTypeApplicationJson},
      {"tracing/json/simple_dump", kContentTypeApplicationJson}};
  return common_endpoints;
}

// Add necessary query params to get 200 response in tests.
const unordered_map<string, string>& GetTServerWebserverEndpoints(const string& tablet_id) {
  static const unordered_map<string, string> tserver_endpoints = {
      {"scans", kContentTypeTextHtml},
      {"tablets", kContentTypeTextHtml},
      {Substitute("tablet?id=$0", tablet_id), kContentTypeTextHtml},
      {"transactions", kContentTypeTextHtml},
      {Substitute("tablet-rowsetlayout-svg?id=$0", tablet_id), kContentTypeTextHtml},
      {Substitute("tablet-consensus-status?id=$0", tablet_id), kContentTypeTextHtml},
      {Substitute("log-anchors?id=$0", tablet_id), kContentTypeTextHtml},
      {"dashboards", kContentTypeTextHtml},
      {"maintenance-manager", kContentTypeTextHtml}};
  return tserver_endpoints;
}

// Add necessary query params to get 200 response in tests.
const unordered_map<string, string>& GetMasterWebserverEndpoints(const string& table_id) {
  static unordered_map<string, string> master_endpoints = {
      {"tablet-servers", kContentTypeTextHtml},
      {"tables", kContentTypeTextHtml},
      {Substitute("table?id=$0", table_id), kContentTypeTextHtml},
      {"masters", kContentTypeTextHtml},
      {"ipki-ca-cert", kContentTypeTextPlain},
      {"ipki-ca-cert-pem", kContentTypeTextPlain},
      {"ipki-ca-cert-der", kContentTypeApplicationOctet},
      {"dump-entities", kContentTypeApplicationJson}};
  return master_endpoints;
}

void CheckPrometheusOutput(const string& prometheus_output) {
  vector<string> lines = strings::Split(prometheus_output, "\n", strings::SkipEmpty());
  vector<vector<string>> metric_groups;
  // Split the lines into groups. Every group contains a help line, a type line and
  // then lines with the actual metric values in this order.
  for (const auto& line : lines) {
    if (HasPrefixString(line, "# HELP")) {
      metric_groups.push_back({line});
    } else if (HasPrefixString(line, "# TYPE")) {
      metric_groups.back().push_back(line);
    } else {
      metric_groups.back().push_back(line);
    }
  }

  std::unordered_set<string> metric_names;
  for (const auto& group : metric_groups) {
    ASSERT_GE(group.size(), 3);
    ASSERT_STR_MATCHES(group[0], "^# HELP ");
    ASSERT_STR_MATCHES(group[1], "^# TYPE ");
    vector<string> help_line_split(strings::Split(group[0], " "));
    vector<string> type_line_split(strings::Split(group[1], " "));
    ASSERT_GE(help_line_split.size(), 3);
    ASSERT_GE(type_line_split.size(), 3);
    string name_from_help_line = help_line_split[2];
    string name_from_type_line = type_line_split[2];
    ASSERT_EQ(name_from_type_line, name_from_help_line);
    ASSERT_TRUE(metric_names.emplace(name_from_help_line).second)
        << "Duplicate metric: " << name_from_help_line;
    for (int i = 2; i < group.size(); i++) {
      ASSERT_TRUE(HasPrefixString(group[i], name_from_help_line))
          << "Every line should start with the expected metric name: " << name_from_help_line;
    }
  }
}
} // namespace kudu
