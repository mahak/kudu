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

#include "kudu/consensus/leader_election.h"

#include <algorithm>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "kudu/common/wire_protocol.h"
#include "kudu/consensus/consensus_peers.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/tserver/tserver.pb.h"
#include "kudu/util/logging.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/status.h"

namespace kudu {
namespace consensus {

using std::string;
using std::unique_ptr;
using std::vector;
using strings::Substitute;

///////////////////////////////////////////////////
// VoteCounter
///////////////////////////////////////////////////

VoteCounter::VoteCounter(int num_voters, int majority_size)
    : num_voters_(num_voters),
      majority_size_(majority_size),
      yes_votes_(0),
      no_votes_(0) {
  CHECK_LE(majority_size, num_voters);
  CHECK_GT(num_voters_, 0);
  CHECK_GT(majority_size_, 0);
}

Status VoteCounter::RegisterVote(const string& voter_uuid, ElectionVote vote,
                                 bool* is_duplicate) {
  // Handle repeated votes.
  if (PREDICT_FALSE(ContainsKey(votes_, voter_uuid))) {
    // Detect changed votes.
    ElectionVote prior_vote = votes_[voter_uuid];
    if (PREDICT_FALSE(prior_vote != vote)) {
      string msg = Substitute("Peer $0 voted a different way twice in the same election. "
                              "First vote: $1, second vote: $2.",
                              voter_uuid, prior_vote, vote);
      return Status::InvalidArgument(msg);
    }

    // This was just a duplicate. Allow the caller to log it but don't change
    // the voting record.
    *is_duplicate = true;
    return Status::OK();
  }

  // Sanity check to ensure we did not exceed the allowed number of voters.
  if (PREDICT_FALSE(yes_votes_ + no_votes_ == num_voters_)) {
    // More unique voters than allowed!
    return Status::InvalidArgument(Substitute(
        "Vote from peer $0 would cause the number of votes to exceed the expected number of "
        "voters, which is $1. Votes already received from the following peers: {$2}",
        voter_uuid,
        num_voters_,
        JoinKeysIterator(votes_.begin(), votes_.end(), ", ")));
  }

  // This is a valid vote, so store it.
  InsertOrDie(&votes_, voter_uuid, vote);
  switch (vote) {
    case VOTE_GRANTED:
      ++yes_votes_;
      break;
    case VOTE_DENIED:
      ++no_votes_;
      break;
  }
  *is_duplicate = false;
  return Status::OK();
}

bool VoteCounter::IsDecided() const {
  return yes_votes_ >= majority_size_ ||
         no_votes_ > num_voters_ - majority_size_;
}

Status VoteCounter::GetDecision(ElectionVote* decision) const {
  if (yes_votes_ >= majority_size_) {
    *decision = VOTE_GRANTED;
    return Status::OK();
  }
  if (no_votes_ > num_voters_ - majority_size_) {
    *decision = VOTE_DENIED;
    return Status::OK();
  }
  return Status::IllegalState("Vote not yet decided");
}

int VoteCounter::GetTotalVotesCounted() const {
  return yes_votes_ + no_votes_;
}

bool VoteCounter::AreAllVotesIn() const {
  return GetTotalVotesCounted() == num_voters_;
}

string VoteCounter::GetElectionSummary() const {
  vector<string> yes_voter_uuids;
  vector<string> no_voter_uuids;
  for (const auto& entry : votes_) {
    switch (entry.second) {
      case VOTE_GRANTED:
        yes_voter_uuids.push_back(entry.first);
        break;
      case VOTE_DENIED:
        no_voter_uuids.push_back(entry.first);
        break;
    }
  }
  return Substitute("received $0 responses out of $1 voters: $2 yes votes; "
                    "$3 no votes. yes voters: $4; no voters: $5",
                    yes_votes_ + no_votes_,
                    num_voters_,
                    yes_votes_,
                    no_votes_,
                    JoinStrings(yes_voter_uuids, ", "),
                    JoinStrings(no_voter_uuids, ", "));
}

///////////////////////////////////////////////////
// ElectionResult
///////////////////////////////////////////////////

ElectionResult::ElectionResult(VoteRequestPB request,
                               ElectionVote election_decision,
                               ConsensusTerm highest_term,
                               string msg,
                               MonoTime op_start_time)
    : vote_request(std::move(request)),
      decision(election_decision),
      highest_voter_term(highest_term),
      message(std::move(msg)),
      start_time(op_start_time) {
  DCHECK(!message.empty());
}

///////////////////////////////////////////////////
// LeaderElection::VoterState
///////////////////////////////////////////////////

string LeaderElection::VoterState::PeerInfo() const {
  string info = peer_uuid;
  if (proxy) {
    strings::SubstituteAndAppend(&info, " ($0)", proxy->PeerName());
  }
  return info;
}

///////////////////////////////////////////////////
// LeaderElection
///////////////////////////////////////////////////

LeaderElection::LeaderElection(RaftConfigPB config,
                               PeerProxyFactory* proxy_factory,
                               VoteRequestPB request,
                               VoteCounter vote_counter,
                               MonoDelta timeout,
                               ElectionDecisionCallback decision_callback)
    : has_responded_(false),
      config_(std::move(config)),
      proxy_factory_(proxy_factory),
      request_(std::move(request)),
      vote_counter_(std::move(vote_counter)),
      timeout_(timeout),
      decision_callback_(std::move(decision_callback)),
      highest_voter_term_(0) {
}

LeaderElection::~LeaderElection() {
  std::lock_guard guard(lock_);
  DCHECK(has_responded_); // We must always call the callback exactly once.
}

void LeaderElection::Run() {
  VLOG_WITH_PREFIX(1) << "Running leader election.";
  start_time_ = MonoTime::Now();

  // Initialize voter state tracking.
  vector<string> other_voter_uuids;
  voter_state_.clear();
  for (const auto& peer : config_.peers()) {
    if (request_.candidate_uuid() == peer.permanent_uuid()) {
      DCHECK_EQ(RaftPeerPB::VOTER, peer.member_type())
          << Substitute("non-voter member $0 tried to start an election; "
                        "Raft config {$1}",
                        peer.permanent_uuid(),
                        pb_util::SecureShortDebugString(config_));
      continue;
    }
    if (peer.member_type() != RaftPeerPB::VOTER) {
      continue;
    }
    other_voter_uuids.emplace_back(peer.permanent_uuid());

    unique_ptr<VoterState> state(new VoterState);
    state->peer_uuid = peer.permanent_uuid();
    state->proxy_status = proxy_factory_->NewProxy(peer, &state->proxy);
    EmplaceOrDie(&voter_state_, peer.permanent_uuid(), std::move(state));
  }

  // Ensure that the candidate has already voted for itself.
  CHECK_EQ(1, vote_counter_.GetTotalVotesCounted()) << "Candidate must vote for itself first";

  // Ensure that existing votes + future votes add up to the expected total.
  CHECK_EQ(vote_counter_.GetTotalVotesCounted() + other_voter_uuids.size(),
           vote_counter_.GetTotalExpectedVotes())
      << "Expected different number of voters. Voter UUIDs: ["
      << JoinStringsIterator(other_voter_uuids.begin(), other_voter_uuids.end(), ", ")
      << "]; RaftConfig: {" << pb_util::SecureShortDebugString(config_) << "}";

  // Check if we have already won the election (relevant if this is a
  // single-node configuration, since we always pre-vote for ourselves).
  CheckForDecision();

  // The rest of the code below is for a typical multi-node configuration.
  vector<string> other_voter_info;
  other_voter_info.reserve(other_voter_uuids.size());
  scoped_refptr<LeaderElection> self(this);
  for (const auto& voter_uuid : other_voter_uuids) {
    VoterState* state = nullptr;
    {
      std::lock_guard guard(lock_);
      state = FindOrDie(voter_state_, voter_uuid).get();
      // Safe to drop the lock because voter_state_ is not mutated outside of
      // the constructor / destructor. We do this to avoid deadlocks below.
    }
    other_voter_info.push_back(state->PeerInfo());

    // If we failed to construct the proxy, just record a 'NO' vote with the status
    // that indicates why it failed.
    if (!state->proxy_status.ok()) {
      LOG_WITH_PREFIX(WARNING) << "Was unable to construct an RPC proxy to peer "
                               << state->PeerInfo() << ": " << state->proxy_status.ToString()
                               << ". Counting it as a 'NO' vote.";
      {
        std::lock_guard guard(lock_);
        RecordVoteUnlocked(*state, VOTE_DENIED);
      }
      CheckForDecision();
      continue;
    }

    // Send the RPC request.
    state->rpc.set_timeout(timeout_);

    state->request = request_;
    state->request.set_dest_uuid(voter_uuid);

    state->proxy->RequestConsensusVoteAsync(
        state->request,
        &state->response,
        &state->rpc,
        [self, voter_uuid]() { self->VoteResponseRpcCallback(voter_uuid); });
  }
  LOG_WITH_PREFIX(INFO) << Substitute("Requested $0vote from peers $1",
                                      request_.is_pre_election() ? "pre-" : "",
                                      JoinStrings(other_voter_info, ", "));
}

void LeaderElection::CheckForDecision() {
  bool to_respond = false;
  {
    std::lock_guard guard(lock_);
    // Check if the vote has been newly decided.
    if (!result_ && vote_counter_.IsDecided()) {
      ElectionVote decision;
      CHECK_OK(vote_counter_.GetDecision(&decision));
      const auto election_won = decision == VOTE_GRANTED;
      LOG_WITH_PREFIX(INFO) << Substitute("Election decided. Result: candidate $0. "
                                          "Election summary: $1",
                                          election_won ? "won" : "lost",
                                          vote_counter_.GetElectionSummary());
      string msg = election_won ?
          "achieved majority votes" : "could not achieve majority";
      result_.reset(new ElectionResult(
          request_, decision, highest_voter_term_, std::move(msg), start_time_));
    }
    // Check whether to respond. This can happen as a result of either getting
    // a majority vote or of something invalidating the election, like
    // observing a higher term.
    if (result_ && !has_responded_) {
      has_responded_ = true;
      to_respond = true;
    }
  }

  // Respond outside of the lock.
  if (to_respond) {
    // This is thread-safe since result_ is write-once.
    decision_callback_(*result_);
  }
}

void LeaderElection::VoteResponseRpcCallback(const string& voter_uuid) {
  {
    std::lock_guard guard(lock_);
    VoterState* state = FindOrDie(voter_state_, voter_uuid).get();

    // Check for RPC errors.
    if (!state->rpc.status().ok()) {
      LOG_WITH_PREFIX(WARNING) << "RPC error from VoteRequest() call to peer "
                               << state->PeerInfo() << ": "
                               << state->rpc.status().ToString();
      RecordVoteUnlocked(*state, VOTE_DENIED);

    // Check for tablet errors.
    } else if (state->response.has_error()) {
      LOG_WITH_PREFIX(WARNING) << "Tablet error from VoteRequest() call to peer "
                               << state->PeerInfo() << ": "
                               << StatusFromPB(state->response.error().status()).ToString();
      RecordVoteUnlocked(*state, VOTE_DENIED);

    // If the peer changed their IP address, we shouldn't count this vote since
    // our knowledge of the configuration is in an inconsistent state.
    } else if (PREDICT_FALSE(voter_uuid != state->response.responder_uuid())) {
      LOG_WITH_PREFIX(DFATAL) << Substitute(
          "$0: peer UUID mismatch from VoteRequest(): expected $1; actual $2",
          state->PeerInfo(), voter_uuid, state->response.responder_uuid());
      RecordVoteUnlocked(*state, VOTE_DENIED);
    } else {
      // No error: count actual votes.
      if (state->response.has_responder_term()) {
        highest_voter_term_ = std::max(highest_voter_term_,
                                       state->response.responder_term());
      }
      if (state->response.vote_granted()) {
        HandleVoteGrantedUnlocked(*state);
      } else {
        HandleVoteDeniedUnlocked(*state);
      }
    }
  }

  // Check for a decision outside the lock.
  CheckForDecision();
}

void LeaderElection::RecordVoteUnlocked(const VoterState& state, ElectionVote vote) {
  DCHECK(lock_.is_locked());

  // Record the vote.
  bool duplicate = false;
  const auto s = vote_counter_.RegisterVote(state.peer_uuid, vote, &duplicate);
  if (!s.ok()) {
    LOG_WITH_PREFIX(WARNING) << "Error registering vote for peer "
                             << state.PeerInfo() << ": " << s.ToString();
    return;
  }
  if (duplicate) {
    // Note: This is DFATAL because at the time of writing we do not support
    // retrying vote requests, so this should be impossible. It may be valid to
    // receive duplicate votes in the future if we implement retry.
    LOG_WITH_PREFIX(DFATAL) << "Duplicate vote received from peer " << state.PeerInfo();
  }
}

void LeaderElection::HandleHigherTermUnlocked(const VoterState& state) {
  DCHECK(lock_.is_locked());
  DCHECK(state.response.has_responder_term());
  DCHECK_GT(state.response.responder_term(), election_term());

  string msg = Substitute("Vote denied by peer $0 with higher term. Message: $1",
                          state.PeerInfo(),
                          StatusFromPB(state.response.consensus_error().status()).ToString());
  LOG_WITH_PREFIX(INFO) << msg;

  if (!result_) {
    LOG_WITH_PREFIX(INFO) << "Cancelling election due to peer responding with higher term";
    result_.reset(new ElectionResult(
        request_, VOTE_DENIED, state.response.responder_term(), std::move(msg), start_time_));
  }
}

void LeaderElection::HandleVoteGrantedUnlocked(const VoterState& state) {
  DCHECK(lock_.is_locked());
  DCHECK(state.response.vote_granted());
  DCHECK(state.response.has_responder_term());
  DCHECK(request_.is_pre_election() ||
         state.response.responder_term() == election_term());
  VLOG_WITH_PREFIX(1) << "Vote granted by peer " << state.PeerInfo();
  RecordVoteUnlocked(state, VOTE_GRANTED);
}

void LeaderElection::HandleVoteDeniedUnlocked(const VoterState& state) {
  DCHECK(lock_.is_locked());
  DCHECK(!state.response.vote_granted());

  // If one of the voters responds with a greater term than our own, and we
  // have not yet triggered the decision callback, it cancels the election.
  if (state.response.has_responder_term() &&
      state.response.responder_term() > election_term()) {
    return HandleHigherTermUnlocked(state);
  }

  VLOG_WITH_PREFIX(1) << Substitute(
      "Vote denied by peer $0. Message: $1",
      state.PeerInfo(),
      StatusFromPB(state.response.consensus_error().status()).ToString());
  RecordVoteUnlocked(state, VOTE_DENIED);
}

string LeaderElection::LogPrefix() const {
  return Substitute("T $0 P $1 [CANDIDATE]: Term $2 $3election: ",
                    request_.tablet_id(),
                    request_.candidate_uuid(),
                    request_.candidate_term(),
                    request_.is_pre_election() ? "pre-" : "");
}

} // namespace consensus
} // namespace kudu
