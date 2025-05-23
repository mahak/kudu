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

#include "kudu/consensus/log_anchor_registry.h"

#include <ostream>
#include <string>
#include <utility>

#include <glog/logging.h>

#include "kudu/consensus/opid_util.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/substitute.h"

namespace kudu {
namespace log {

using consensus::kInvalidOpIdIndex;
using std::string;
using strings::Substitute;
using strings::SubstituteAndAppend;

LogAnchorRegistry::LogAnchorRegistry() {
}

LogAnchorRegistry::~LogAnchorRegistry() {
  CHECK(anchors_.empty());
}

void LogAnchorRegistry::Register(int64_t log_index,
                                 const string& owner,
                                 LogAnchor* anchor) {
  std::lock_guard l(lock_);
  RegisterUnlocked(log_index, owner, anchor);
}

Status LogAnchorRegistry::RegisterOrUpdate(int64_t log_index,
                                           const std::string& owner,
                                           LogAnchor* anchor) {
  std::lock_guard l(lock_);
  if (anchor->is_registered) {
    RETURN_NOT_OK(UnregisterUnlocked(anchor));
  }
  RegisterUnlocked(log_index, owner, anchor);
  return Status::OK();
}

Status LogAnchorRegistry::Unregister(LogAnchor* anchor) {
  std::lock_guard l(lock_);
  return UnregisterUnlocked(anchor);
}

Status LogAnchorRegistry::UnregisterIfAnchored(LogAnchor* anchor) {
  std::lock_guard l(lock_);
  if (!anchor->is_registered) return Status::OK();
  return UnregisterUnlocked(anchor);
}

Status LogAnchorRegistry::GetEarliestRegisteredLogIndex(int64_t* log_index) {
  std::lock_guard l(lock_);
  auto iter = anchors_.begin();
  if (iter == anchors_.end()) {
    return Status::NotFound("No anchors in registry");
  }

  // Since this is a sorted map, the first element is the one we want.
  *log_index = iter->first;
  return Status::OK();
}

size_t LogAnchorRegistry::GetAnchorCountForTests() const {
  std::lock_guard l(lock_);
  return anchors_.size();
}

std::string LogAnchorRegistry::DumpAnchorInfo() const {
  string buf;
  std::lock_guard l(lock_);
  MonoTime now = MonoTime::Now();
  for (const AnchorMultiMap::value_type& entry : anchors_) {
    const LogAnchor* anchor = entry.second;
    DCHECK(anchor->is_registered);
    if (!buf.empty()) buf += ", ";
    SubstituteAndAppend(&buf, "LogAnchor[index=$0, age=$1s, owner=$2]",
                        anchor->log_index,
                        (now - anchor->when_registered).ToSeconds(),
                        anchor->owner);
  }
  return buf;
}

void LogAnchorRegistry::RegisterUnlocked(int64_t log_index,
                                         const std::string& owner,
                                         LogAnchor* anchor) {
  DCHECK(anchor != nullptr);
  DCHECK(!anchor->is_registered);

  anchor->log_index = log_index;
  anchor->owner.assign(owner);
  anchor->is_registered = true;
  anchor->when_registered = MonoTime::Now();
  AnchorMultiMap::value_type value(log_index, anchor);
  anchors_.insert(value);
}

Status LogAnchorRegistry::UnregisterUnlocked(LogAnchor* anchor) {
  DCHECK(anchor != nullptr);
  DCHECK(anchor->is_registered);

  auto iter = anchors_.find(anchor->log_index);
  while (iter != anchors_.end()) {
    if (iter->second == anchor) {
      anchor->is_registered = false;
      anchors_.erase(iter);
      // No need for the iterator to remain valid since we return here.
      return Status::OK();
    }
    ++iter;
  }
  return Status::NotFound(Substitute("Anchor with index $0 and owner $1 not found",
                                     anchor->log_index, anchor->owner));
}

LogAnchor::LogAnchor()
  : is_registered(false),
    log_index(kInvalidOpIdIndex) {
}

LogAnchor::~LogAnchor() {
  CHECK(!is_registered) << "Attempted to destruct a registered LogAnchor";
}

MinLogIndexAnchorer::MinLogIndexAnchorer(LogAnchorRegistry* registry,
                                         string owner)
    : registry_(DCHECK_NOTNULL(registry)),
      owner_(std::move(owner)),
      minimum_log_index_(kInvalidOpIdIndex) {}

MinLogIndexAnchorer::~MinLogIndexAnchorer() {
  CHECK_OK(ReleaseAnchor());
}

Status MinLogIndexAnchorer::AnchorIfMinimum(int64_t log_index) {
  std::lock_guard l(lock_);
  if (log_index < minimum_log_index_ ||
      PREDICT_FALSE(minimum_log_index_ == kInvalidOpIdIndex)) {
    minimum_log_index_ = log_index;
    return registry_->RegisterOrUpdate(minimum_log_index_, owner_, &anchor_);
  }
  return Status::OK();
}

Status MinLogIndexAnchorer::ReleaseAnchor() {
  std::lock_guard l(lock_);
  if (PREDICT_TRUE(minimum_log_index_ != kInvalidOpIdIndex)) {
    return registry_->Unregister(&anchor_);
  }
  return Status::OK(); // If there were no inserts, return OK.
}

int64_t MinLogIndexAnchorer::minimum_log_index() const {
  std::lock_guard l(lock_);
  return minimum_log_index_;
}

} // namespace log
} // namespace kudu
