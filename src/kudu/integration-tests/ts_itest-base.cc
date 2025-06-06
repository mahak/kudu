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

#include "kudu/integration-tests/ts_itest-base.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <ostream>
#include <random>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/client/client.h"
#include "kudu/client/schema.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/strings/split.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/integration-tests/cluster_itest_util.h"
#include "kudu/integration-tests/cluster_verifier.h"
#include "kudu/integration-tests/mini_cluster_fs_inspector.h"
#include "kudu/master/master.pb.h"
#include "kudu/master/master.proxy.h" // IWYU pragma: keep
#include "kudu/mini-cluster/external_mini_cluster.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/tserver/tablet_server-test-base.h"
#include "kudu/tserver/tserver.pb.h"
#include "kudu/tserver/tserver_service.proxy.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

DECLARE_int32(consensus_rpc_timeout_ms);

DEFINE_string(ts_flags, "", "Flags to pass through to tablet servers");
DEFINE_string(master_flags, "", "Flags to pass through to masters");

DEFINE_int32(num_tablet_servers, 3, "Number of tablet servers to start");
DEFINE_int32(num_replicas, 3, "Number of replicas per tablet server");

using kudu::client::sp::shared_ptr;
using kudu::itest::TServerDetails;
using kudu::cluster::ExternalTabletServer;
using kudu::cluster::LocationInfo;
using std::pair;
using std::set;
using std::string;
using std::unique_ptr;
using std::unordered_multimap;
using std::unordered_set;
using std::vector;
using strings::Split;
using strings::Substitute;

namespace kudu {
namespace tserver {

static const int kMaxRetries = 20;

TabletServerIntegrationTestBase::TabletServerIntegrationTestBase()
    : random_(SeedRandom()) {
}

void TabletServerIntegrationTestBase::SetUp() {
  TabletServerTestBase::SetUp();
}

void TabletServerIntegrationTestBase::AddExtraFlags(
    const string& flags_str, vector<string>* flags) {
  if (flags_str.empty()) {
    return;
  }
  vector<string> split_flags = Split(flags_str, " ");
  for (const string& flag : split_flags) {
    flags->push_back(flag);
  }
}

void TabletServerIntegrationTestBase::CreateCluster(
    const std::string& data_root_path,
    vector<string> non_default_ts_flags,
    vector<string> non_default_master_flags,
    cluster::LocationInfo location_info) {

  LOG(INFO) << "Starting cluster with:";
  LOG(INFO) << "--------------";
  LOG(INFO) << FLAGS_num_tablet_servers << " tablet servers";
  LOG(INFO) << FLAGS_num_replicas << " replicas per TS";
  LOG(INFO) << "--------------";

  cluster::ExternalMiniClusterOptions opts;
  opts.num_tablet_servers = FLAGS_num_tablet_servers;
  opts.cluster_root = GetTestPath(data_root_path);
  opts.location_info = std::move(location_info);

  // Enable exactly once semantics for tests.

  // If the caller passed no flags use the default ones, where we stress
  // consensus by setting low timeouts and frequent cache misses.
  if (non_default_ts_flags.empty()) {
    opts.extra_tserver_flags.emplace_back("--log_cache_size_limit_mb=10");
    opts.extra_tserver_flags.push_back(
        Substitute("--consensus_rpc_timeout_ms=$0",
                   FLAGS_consensus_rpc_timeout_ms));
  } else {
    for (auto& flag : non_default_ts_flags) {
      opts.extra_tserver_flags.emplace_back(std::move(flag));
    }
  }
  for (auto& flag : non_default_master_flags) {
    opts.extra_master_flags.emplace_back(std::move(flag));
  }

  AddExtraFlags(FLAGS_ts_flags, &opts.extra_tserver_flags);
  AddExtraFlags(FLAGS_master_flags, &opts.extra_master_flags);

  cluster_.reset(new cluster::ExternalMiniCluster(std::move(opts)));
  ASSERT_OK(cluster_->Start());
  inspect_.reset(new itest::MiniClusterFsInspector(cluster_.get()));
  CreateTSProxies();
}

// Creates TSServerDetails instance for each TabletServer and stores them
// in 'tablet_servers_'.
void TabletServerIntegrationTestBase::CreateTSProxies() {
  CHECK(tablet_servers_.empty());
  CHECK_OK(itest::CreateTabletServerMap(cluster_->master_proxy(),
                                        client_messenger_,
                                        &tablet_servers_));
}

// Waits for all the replicas of all tablets of 'table_id' table to become
// online and populates the tablet_replicas_ map.
Status TabletServerIntegrationTestBase::WaitForReplicasAndUpdateLocations(
    const string& table_id) {

  const size_t num_replicas_total = FLAGS_num_replicas;
  bool replicas_missing = true;
  for (int num_retries = 0; replicas_missing && num_retries < kMaxRetries; num_retries++) {
    unordered_multimap<string, TServerDetails*> tablet_replicas;
    master::GetTableLocationsRequestPB req;
    master::GetTableLocationsResponsePB resp;
    rpc::RpcController controller;
    req.mutable_table()->set_table_name(table_id);
    req.set_replica_type_filter(master::ANY_REPLICA);
    req.set_intern_ts_infos_in_response(true);
    controller.set_timeout(MonoDelta::FromSeconds(1));
    RETURN_NOT_OK(cluster_->master_proxy()->GetTableLocations(req, &resp, &controller));
    RETURN_NOT_OK(controller.status());
    if (resp.has_error()) {
      switch (resp.error().code()) {
        case master::MasterErrorPB::TABLET_NOT_RUNNING:
          LOG(WARNING)<< "At least one tablet is not yet running";
          break;

        case master::MasterErrorPB::NOT_THE_LEADER:   // fallthrough
        case master::MasterErrorPB::CATALOG_MANAGER_NOT_INITIALIZED:
          LOG(WARNING)<< "CatalogManager is not yet ready to serve requests";
          break;

        default:
          LOG(ERROR) << "Response had a fatal error: "
                     << pb_util::SecureShortDebugString(resp.error());
          return StatusFromPB(resp.error().status());
      }
      SleepFor(MonoDelta::FromSeconds(1));
      continue;
    }

    for (const auto& location : resp.tablet_locations()) {
      for (const auto& replica : location.interned_replicas()) {
        TServerDetails* server =
            FindOrDie(tablet_servers_, resp.ts_infos(replica.ts_info_idx()).permanent_uuid());
        tablet_replicas.emplace(location.tablet_id(), server);
      }

      const size_t num_replicas_found = tablet_replicas.count(location.tablet_id());
      if (num_replicas_found < num_replicas_total) {
        LOG(WARNING)<< Substitute(
            "found only $0 out of $1 replicas of tablet $2: $3",
            num_replicas_found, num_replicas_total,
            location.tablet_id(), pb_util::SecureShortDebugString(location));
        replicas_missing = true;
        SleepFor(MonoDelta::FromSeconds(1));
        break;
      }

      replicas_missing = false;
    }
    if (!replicas_missing) {
      tablet_replicas_.swap(tablet_replicas);
    }
  }

  if (replicas_missing) {
    return Status::NotFound(Substitute(
        "not all replicas of tablets comprising table $0 are registered yet",
        table_id));
  }

  // GetTableLocations() does not guarantee that all replicas are actually
  // running. Some may still be bootstrapping. Wait for them before
  // returning.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ExternalTabletServer* ts = cluster_->tablet_server(i);
    int expected_tablet_count = 0;
    for (const auto& [_, ts_details] : tablet_replicas_) {
      if (ts->uuid() == ts_details->uuid()) {
        ++expected_tablet_count;
      }
    }
    if (expected_tablet_count == 0) {
      // Nothing to wait for.
      continue;
    }
    LOG(INFO) << Substitute(
        "Waiting for $0 tablets on tserver $1 to finish bootstrapping",
        expected_tablet_count, ts->uuid());
    RETURN_NOT_OK(cluster_->WaitForTabletsRunning(
        ts, expected_tablet_count, MonoDelta::FromSeconds(20)));
  }
  return Status::OK();
}

// Returns the last committed leader of the consensus configuration. Tries to get it from master
// but then actually tries to the get the committed consensus configuration to make sure.
TServerDetails* TabletServerIntegrationTestBase::GetLeaderReplicaOrNull(
    const string& tablet_id) {
  string leader_uuid;
  Status master_found_leader_result = GetTabletLeaderUUIDFromMaster(
      tablet_id, &leader_uuid);

  // See if the master is up to date. I.e. if it does report a leader and if the
  // replica it reports as leader is still alive and (at least thinks) its still
  // the leader.
  TServerDetails* leader;
  if (master_found_leader_result.ok()) {
    leader = GetReplicaWithUuidOrNull(tablet_id, leader_uuid);
    if (leader && itest::GetReplicaStatusAndCheckIfLeader(
          leader, tablet_id, MonoDelta::FromMilliseconds(100)).ok()) {
      return leader;
    }
  }

  // The replica we got from the master (if any) is either dead or not the leader.
  // Find the actual leader.
  pair<itest::TabletReplicaMap::iterator, itest::TabletReplicaMap::iterator> range =
      tablet_replicas_.equal_range(tablet_id);
  vector<TServerDetails*> replicas_copy;
  for (;range.first != range.second; ++range.first) {
    replicas_copy.push_back((*range.first).second);
  }

  std::mt19937 gen(SeedRandom());
  std::shuffle(replicas_copy.begin(), replicas_copy.end(), gen);
  for (TServerDetails* replica : replicas_copy) {
    if (itest::GetReplicaStatusAndCheckIfLeader(
          replica, tablet_id, MonoDelta::FromMilliseconds(100)).ok()) {
      return replica;
    }
  }
  return nullptr;
}

// For the last committed consensus configuration, return the last committed
// leader of the consensus configuration and its followers.
Status TabletServerIntegrationTestBase::GetTabletLeaderAndFollowers(
    const string& tablet_id,
    TServerDetails** leader,
    vector<TServerDetails*>* followers) {

  pair<itest::TabletReplicaMap::iterator, itest::TabletReplicaMap::iterator> range =
      tablet_replicas_.equal_range(tablet_id);
  vector<TServerDetails*> replicas;
  for (; range.first != range.second; ++range.first) {
    replicas.push_back((*range.first).second);
  }

  TServerDetails* leader_replica = nullptr;
  auto it = replicas.begin();
  for (; it != replicas.end(); ++it) {
    TServerDetails* replica = *it;
    bool found_leader_replica = false;
    for (auto i = 0; i < kMaxRetries; ++i) {
      if (itest::GetReplicaStatusAndCheckIfLeader(
            replica, tablet_id, MonoDelta::FromMilliseconds(100)).ok()) {
        leader_replica = replica;
        found_leader_replica = true;
        break;
      }
    }
    if (found_leader_replica) {
      break;
    }
  }
  if (!leader_replica) {
    return Status::NotFound("leader replica not found");
  }

  if (leader) {
    *leader = leader_replica;
  }
  if (followers) {
    CHECK(replicas.end() != it);
    replicas.erase(it);
    followers->swap(replicas);
  }
  return Status::OK();
}

Status TabletServerIntegrationTestBase::GetLeaderReplicaWithRetries(
    const string& tablet_id,
    TServerDetails** leader,
    int max_attempts) {
  int attempts = 0;
  while (attempts < max_attempts) {
    *leader = GetLeaderReplicaOrNull(tablet_id);
    if (*leader) {
      return Status::OK();
    }
    attempts++;
    SleepFor(MonoDelta::FromMilliseconds(100L * attempts));
  }
  return Status::NotFound("leader replica not found");
}

Status TabletServerIntegrationTestBase::GetTabletLeaderUUIDFromMaster(
    const string& tablet_id, string* leader_uuid) {
  master::GetTableLocationsRequestPB req;
  master::GetTableLocationsResponsePB resp;
  rpc::RpcController controller;
  controller.set_timeout(MonoDelta::FromMilliseconds(100));
  req.mutable_table()->set_table_name(kTableId);
  req.set_intern_ts_infos_in_response(true);

  RETURN_NOT_OK(cluster_->master_proxy()->GetTableLocations(req, &resp, &controller));
  for (const master::TabletLocationsPB& loc : resp.tablet_locations()) {
    if (loc.tablet_id() == tablet_id) {
      for (const auto& replica : loc.interned_replicas()) {
        if (replica.role() == consensus::RaftPeerPB::LEADER) {
          *leader_uuid = resp.ts_infos(replica.ts_info_idx()).permanent_uuid();
          return Status::OK();
        }
      }
    }
  }
  return Status::NotFound("Unable to find leader for tablet", tablet_id);
}

TServerDetails* TabletServerIntegrationTestBase::GetReplicaWithUuidOrNull(
    const string& tablet_id, const string& uuid) {
  pair<itest::TabletReplicaMap::iterator, itest::TabletReplicaMap::iterator> range =
      tablet_replicas_.equal_range(tablet_id);
  for (;range.first != range.second; ++range.first) {
    if ((*range.first).second->instance_id.permanent_uuid() == uuid) {
      return (*range.first).second;
    }
  }
  return nullptr;
}

Status TabletServerIntegrationTestBase::WaitForTabletServers() {
  const auto num_ts = FLAGS_num_tablet_servers;
  int num_retries = 0;
  while (true) {
    if (num_retries >= kMaxRetries) {
      return Status::TimedOut(Substitute(
          "Reached maximum number of retries ($0) while "
          "waiting for all $1 tablet servers to register with master(s)",
          kMaxRetries, num_ts));
    }

    Status status = cluster_->WaitForTabletServerCount(
        num_ts, MonoDelta::FromSeconds(5));
    if (status.IsTimedOut()) {
      LOG(WARNING)<< "Timeout waiting for all replicas to be online, retrying...";
      num_retries++;
      continue;
    }
    break;
  }
  return Status::OK();
}

// Gets the the locations of the consensus configuration and waits until all replicas
// are available for all tablets.
Status TabletServerIntegrationTestBase::WaitForTSAndReplicas(const string& table_id) {
  RETURN_NOT_OK(WaitForTabletServers());
  return WaitForReplicasAndUpdateLocations(table_id);
}

// Removes a set of servers from the replicas_ list.
// Handy for controlling who to validate against after killing servers.
void TabletServerIntegrationTestBase::PruneFromReplicas(
    const unordered_set<string>& uuids) {
  auto iter = tablet_replicas_.begin();
  while (iter != tablet_replicas_.end()) {
    if (uuids.count((*iter).second->instance_id.permanent_uuid()) != 0) {
      iter = tablet_replicas_.erase(iter);
      continue;
    }
    ++iter;
  }

  for (const string& uuid : uuids) {
    delete EraseKeyReturnValuePtr(&tablet_servers_, uuid);
  }
}

void TabletServerIntegrationTestBase::GetOnlyLiveFollowerReplicas(
    const string& tablet_id, vector<TServerDetails*>* followers) {
  followers->clear();
  TServerDetails* leader;
  CHECK_OK(GetLeaderReplicaWithRetries(tablet_id, &leader));

  vector<TServerDetails*> replicas;
  pair<itest::TabletReplicaMap::iterator, itest::TabletReplicaMap::iterator> range =
      tablet_replicas_.equal_range(tablet_id);
  for (;range.first != range.second; ++range.first) {
    replicas.push_back((*range.first).second);
  }

  for (TServerDetails* replica : replicas) {
    if (leader != nullptr &&
        replica->instance_id.permanent_uuid() == leader->instance_id.permanent_uuid()) {
      continue;
    }
    Status s = itest::GetReplicaStatusAndCheckIfLeader(
        replica, tablet_id, MonoDelta::FromMilliseconds(100));
    if (s.IsIllegalState()) {
      followers->push_back(replica);
    }
  }
}

Status TabletServerIntegrationTestBase::ShutdownServerWithUUID(const string& uuid) {
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ExternalTabletServer* ts = cluster_->tablet_server(i);
    if (ts->instance_id().permanent_uuid() == uuid) {
      ts->Shutdown();
      return Status::OK();
    }
  }
  return Status::NotFound("Unable to find server with UUID", uuid);
}

Status TabletServerIntegrationTestBase::RestartServerWithUUID(const string& uuid) {
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ExternalTabletServer* ts = cluster_->tablet_server(i);
    if (ts->instance_id().permanent_uuid() == uuid) {
      ts->Shutdown();
      RETURN_NOT_OK(CheckTabletServersAreAlive(tablet_servers_.size()-1));
      RETURN_NOT_OK(ts->Restart());
      RETURN_NOT_OK(CheckTabletServersAreAlive(tablet_servers_.size()));
      return Status::OK();
    }
  }
  return Status::NotFound("Unable to find server with UUID", uuid);
}

// Since we're fault-tolerant we might mask when a tablet server is
// dead. This returns Status::IllegalState() if fewer than 'num_tablet_servers'
// are alive.
Status TabletServerIntegrationTestBase::CheckTabletServersAreAlive(int num_tablet_servers) {
  int live_count = 0;
  string error = Substitute("Fewer than $0 TabletServers were alive. Dead TSs: ",
                            num_tablet_servers);
  rpc::RpcController controller;
  for (const itest::TabletServerMap::value_type& entry : tablet_servers_) {
    controller.Reset();
    controller.set_timeout(MonoDelta::FromSeconds(10));
    PingRequestPB req;
    PingResponsePB resp;
    Status s = entry.second->tserver_proxy->Ping(req, &resp, &controller);
    if (!s.ok()) {
      error += "\n" + entry.second->ToString() +  " (" + s.ToString() + ")";
      continue;
    }
    live_count++;
  }
  if (live_count < num_tablet_servers) {
    return Status::IllegalState(error);
  }
  return Status::OK();
}

void TabletServerIntegrationTestBase::TearDown() {
  STLDeleteValues(&tablet_servers_);
  TabletServerTestBase::TearDown();
}

void TabletServerIntegrationTestBase::CreateClient(shared_ptr<client::KuduClient>* client) {
  // Connect to the cluster.
  ASSERT_OK(client::KuduClientBuilder()
            .add_master_server_addr(cluster_->master()->bound_rpc_addr().ToString())
            .Build(client));
}

// Create a table with a single tablet, with 'num_replicas'.
void TabletServerIntegrationTestBase::CreateTable(const string& table_id) {
  // The tests here make extensive use of server schemas, but we need
  // a client schema to create the table.
  client::KuduSchema client_schema(client::KuduSchema::FromSchema(schema_));
  unique_ptr<client::KuduTableCreator> table_creator(client_->NewTableCreator());
  ASSERT_OK(table_creator->table_name(table_id)
                .schema(&client_schema)
                .set_range_partition_columns({"key"})
                .num_replicas(FLAGS_num_replicas)
                .set_owner("alice")
                .Create());
  ASSERT_OK(client_->OpenTable(table_id, &table_));
}

void TabletServerIntegrationTestBase::BuildAndStart(
    vector<string> ts_flags,
    vector<string> master_flags,
    LocationInfo location_info,
    bool create_table) {
  NO_FATALS(CreateCluster("raft_consensus-itest-cluster",
                          std::move(ts_flags), std::move(master_flags),
                          std::move(location_info)));
  NO_FATALS(CreateClient(&client_));
  if (create_table) {
    NO_FATALS(CreateTable());
    ASSERT_OK(WaitForTSAndReplicas());
    ASSERT_FALSE(tablet_replicas_.empty());
    tablet_id_ = tablet_replicas_.begin()->first;
  } else {
    ASSERT_OK(WaitForTabletServers());
  }
}

void TabletServerIntegrationTestBase::AssertAllReplicasAgree(int expected_result_count) {
  ClusterVerifier v(cluster_.get());
  NO_FATALS(v.CheckCluster());
  NO_FATALS(v.CheckRowCount(kTableId, ClusterVerifier::EXACTLY, expected_result_count));
}

// Check for and restart any TS that have crashed.
// Returns the number of servers restarted.
int TabletServerIntegrationTestBase::RestartAnyCrashedTabletServers() {
  int restarted = 0;
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    if (!cluster_->tablet_server(i)->IsProcessAlive()) {
      LOG(INFO) << "TS " << i << " appears to have crashed. Restarting.";
      cluster_->tablet_server(i)->Shutdown();
      CHECK_OK(cluster_->tablet_server(i)->Restart());
      restarted++;
    }
  }
  return restarted;
}

// Assert that no tablet servers have crashed.
// Tablet servers that have been manually Shutdown() are allowed.
void TabletServerIntegrationTestBase::AssertNoTabletServersCrashed() {
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    if (cluster_->tablet_server(i)->IsShutdown()) {
      continue;
    }
    ASSERT_TRUE(cluster_->tablet_server(i)->IsProcessAlive())
        << "Tablet server " << i << " crashed";
  }
}

Status TabletServerIntegrationTestBase::WaitForLeaderWithCommittedOp(
    const string& tablet_id,
    const MonoDelta& timeout,
    TServerDetails** leader) {
  TServerDetails* leader_res = nullptr;
  RETURN_NOT_OK(GetLeaderReplicaWithRetries(tablet_id, &leader_res));

  RETURN_NOT_OK(WaitForOpFromCurrentTerm(leader_res, tablet_id,
                                         consensus::COMMITTED_OPID, timeout));
  *leader = leader_res;
  return Status::OK();
}

vector<string> TabletServerIntegrationTestBase::GetServersWithReplica(
    const string& tablet_id) const {
  std::set<string> uuids;
  for (const auto& e : tablet_replicas_) {
    if (e.first == tablet_id) {
      uuids.insert(e.second->uuid());
    }
  }
  return vector<string>(uuids.begin(), uuids.end());
}

vector<string> TabletServerIntegrationTestBase::GetServersWithoutReplica(
    const string& tablet_id) const {
  std::set<string> uuids;
  for (const auto& e : tablet_servers_) {
    uuids.insert(e.first);
  }
  for (const auto& e : tablet_replicas_) {
    if (e.first == tablet_id) {
      uuids.erase(e.second->uuid());
    }
  }
  return vector<string>(uuids.begin(), uuids.end());
}

}  // namespace tserver
}  // namespace kudu
