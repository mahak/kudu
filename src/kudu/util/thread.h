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
#pragma once

#include <pthread.h>
#if defined(__linux__)
#include <syscall.h>
#else
#include <sys/syscall.h>
#endif
#include <unistd.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "kudu/gutil/atomicops.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/util/countdown_latch.h"
#include "kudu/util/status.h"

namespace kudu {

class MetricEntity;
class Thread;
class WebCallbackRegistry;

// Utility to join on a thread, printing warning messages if it
// takes too long. For example:
//
//   ThreadJoiner(&my_thread)
//     .warn_after_ms(1000)
//     .warn_every_ms(5000)
//     .Join();
//
// TODO: would be nice to offer a way to use ptrace() or signals to
// dump the stack trace of the thread we're trying to join on if it
// gets stuck. But, after looking for 20 minutes or so, it seems
// pretty complicated to get right.
class ThreadJoiner {
 public:
  explicit ThreadJoiner(Thread* thread);

  // Start emitting warnings after this many milliseconds.
  //
  // Default: 1000 ms.
  ThreadJoiner& warn_after_ms(int ms);

  // After the warnings after started, emit another warning at the
  // given interval.
  //
  // Default: 1000 ms.
  ThreadJoiner& warn_every_ms(int ms);

  // If the thread has not stopped after this number of milliseconds, give up
  // joining on it and return Status::Aborted.
  //
  // -1 (the default) means to wait forever trying to join.
  ThreadJoiner& give_up_after_ms(int ms);

  // Join the thread, subject to the above parameters. If the thread joining
  // fails for any reason, returns RuntimeError. If it times out, returns
  // Aborted.
  Status Join();

 private:
  enum {
    kDefaultWarnAfterMs = 1000,
    kDefaultWarnEveryMs = 1000,
    kDefaultGiveUpAfterMs = -1 // forever
  };

  Thread* thread_;

  int warn_after_ms_;
  int warn_every_ms_;
  int give_up_after_ms_;

  DISALLOW_COPY_AND_ASSIGN(ThreadJoiner);
};

// Thin wrapper around pthread that can register itself with the singleton ThreadMgr
// (a private class implemented in thread.cc entirely, which tracks all live threads so
// that they may be monitored via the debug webpages). This class has a limited subset of
// boost::thread's API. Construction is almost the same, but clients must supply a
// category and a name for each thread so that they can be identified in the debug web
// UI. Otherwise, Join() is the only supported method from boost::thread.
//
// Each Thread object knows its operating system thread ID (TID), which can be used to
// attach debuggers to specific threads, to retrieve resource-usage statistics from the
// operating system, and to assign threads to resource control groups.
//
// Threads are shared objects, but in a degenerate way. They may only have
// up to two referents: the caller that created the thread (parent), and
// the thread itself (child). Moreover, the only two methods to mutate state
// (Join() and the destructor) are constrained: the child may not Join() on
// itself, and the destructor is only run when there's one referent left.
// These constraints allow us to access thread internals without any locks.
class Thread : public RefCountedThreadSafe<Thread> {
 public:

  // Flags passed to Thread::CreateWithFlags().
  enum CreateFlags {
    NO_FLAGS = 0,

    // Disable the use of KernelStackWatchdog to detect and log slow
    // thread creations. This is necessary when starting the kernel stack
    // watchdog thread itself to avoid reentrancy.
    NO_STACK_WATCHDOG = 1 << 0
  };

  // Creates and starts a new thread.
  //  - category: string identifying the thread category to which this thread
  //    belongs, used for organising threads together on the debug UI.
  //  - name: name of this thread. Will be appended with "-<thread-id>" to
  //    ensure uniqueness.
  //  - f: function passed to the constructor and executed immediately in the
  //    separate thread.
  //  - holder: optional shared pointer to hold a reference to the created thread.
  static Status CreateWithFlags(std::string category, std::string name,
                                std::function<void()> f, uint64_t flags,
                                scoped_refptr<Thread>* holder) {
    return StartThread(std::move(category), std::move(name), std::move(f),
                       flags, holder);

  }
  static Status Create(std::string category, std::string name,
                       std::function<void()> f,
                       scoped_refptr<Thread>* holder) {
    return StartThread(std::move(category), std::move(name), std::move(f),
                       NO_FLAGS, holder);
  }

  // Emulates boost::thread and detaches.
  ~Thread();

  // Blocks until this thread finishes execution. Once this method returns, the thread
  // will be unregistered with the ThreadMgr and will not appear in the debug UI.
  void Join();

  // A thread's OS-specific TID is assigned after it start running. However,
  // in order to improve the performance of thread creation, the parent
  // thread does not wait for the child thread to start running before
  // Create() returns. Therefore, when the parent thread finishes Create(),
  // the child thread may not have a OS-specific TID (because it has not
  // actually started execution).
  //
  // In order to get the correct tid, this method spins until the child
  // thread gets the TID.
  int64_t tid() const {
    int64_t t = base::subtle::Acquire_Load(&tid_);
    if (t != PARENT_WAITING_TID) {
      return tid_;
    }
    return WaitForTid();
  }

  // Returns the thread's pthread ID.
  pthread_t pthread_id() const { return thread_; }

  const std::string& name() const { return name_; }
  const std::string& category() const { return category_; }

  // Return a string representation of the thread identifying information.
  std::string ToString() const;

  // The current thread of execution, or NULL if the current thread isn't a kudu::Thread.
  // This call is signal-safe.
  static Thread* current_thread() { return tls_; }

  // Returns a unique, stable identifier for this thread. Note that this is a static
  // method and thus can be used on any thread, including the main thread of the
  // process.
  //
  // In general, this should be used when a value is required that is unique to
  // a thread and must work on any thread including the main process thread.
  //
  // NOTE: this is _not_ the TID, but rather a unique value assigned by the
  // thread implementation. So, this value should not be presented to the user
  // in log messages, etc.
  static int64_t UniqueThreadId() {
#if defined(__linux__)
    // This cast is a little bit ugly, but it is significantly faster than
    // calling syscall(SYS_gettid). In particular, this speeds up some code
    // paths in the tracing implementation.
    return static_cast<int64_t>(pthread_self());
#elif defined(__APPLE__)
    uint64_t tid;
    CHECK_EQ(0, pthread_threadid_np(NULL, &tid));
    return tid;
#else
#error Unsupported platform
#endif
  }

  // Returns the system thread ID (tid on Linux) for the current thread. Note
  // that this is a static method and thus can be used from any thread,
  // including the main thread of the process. This is in contrast to
  // Thread::tid(), which only works on kudu::Threads.
  //
  // Thread::tid() will return the same value, but the value is cached in the
  // Thread object, so will be faster to call.
  //
  // Thread::UniqueThreadId() (or Thread::tid()) should be preferred for
  // performance sensistive code, however it is only guaranteed to return a
  // unique and stable thread ID, not necessarily the system thread ID.
  static int64_t CurrentThreadId() {
#if defined(__linux__)
    return syscall(SYS_gettid);
#else
    return UniqueThreadId();
#endif
  }

 private:
  friend class ThreadJoiner;

  // See 'tid_' docs.
  enum {
    INVALID_TID = -1,
    PARENT_WAITING_TID = -2,
  };

  Thread(std::string category, std::string name, std::function<void()> functor)
      : thread_(0),
        category_(std::move(category)),
        name_(std::move(name)),
        tid_(INVALID_TID),
        functor_(std::move(functor)),
        done_(1),
        joinable_(false) {}

  // Library-specific thread ID.
  pthread_t thread_;

  // Name and category for this thread.
  const std::string category_;
  const std::string name_;

  // OS-specific thread ID.
  //
  // The tid_ member goes through the following states:
  // 1. INVALID_TID: the thread has not been started, or has already exited.
  // 2. PARENT_WAITING_TID: the parent has started the thread, but the
  //    thread has not yet begun running. Therefore the TID is not yet known
  //    but it will be set once the thread starts.
  // 3. <positive value>: the thread is running.
  int64_t tid_;

  // User function to be executed by this thread.
  const std::function<void()> functor_;

  // Joiners wait on this latch to be notified if the thread is done.
  //
  // Note that Joiners must additionally pthread_join(), otherwise certain
  // resources that callers expect to be destroyed (like TLS) may still be
  // alive when a Joiner finishes.
  CountDownLatch done_;

  bool joinable_;

  // Thread local pointer to the current thread of execution. Will be NULL if the current
  // thread is not a Thread.
  static __thread Thread* tls_;

  // Wait for the running thread to publish its tid.
  int64_t WaitForTid() const;

  // Starts the thread running SuperviseThread(), and returns once that thread has
  // initialised and its TID has been read. Waits for notification from the started
  // thread that initialisation is complete before returning. On success, stores a
  // reference to the thread into the 'holder' parameter which can be passed as
  // 'nullptr' if the reference isn't needed.
  static Status StartThread(std::string category, std::string name,
                            std::function<void()> functor, uint64_t flags,
                            scoped_refptr<Thread>* holder);

  // Wrapper for the user-supplied function. Invoked from the new thread,
  // with the Thread as its only argument. Executes functor_, but before
  // doing so registers with the global ThreadMgr and reads the thread's
  // system ID. After functor_ terminates, unregisters with the ThreadMgr.
  // Always returns NULL.
  //
  // The arg parameter is a bare pointer of Thread object, but its reference
  // count has already been incremented in StartThread(), so it is safe to
  // refer to it even after the parent thread drop its reference.
  static void* SuperviseThread(void* arg);

  // Invoked when the user-supplied function finishes or in the case of an
  // abrupt exit (i.e. pthread_exit()). Cleans up after SuperviseThread().
  static void FinishThread(void* arg);
};

// Registers /threadz with the debug webserver, and creates thread-tracking metrics under
// the given entity. If 'web' is NULL, does not register the path handler.
Status StartThreadInstrumentation(const scoped_refptr<MetricEntity>& server_metrics,
                                  WebCallbackRegistry* web);
} // namespace kudu
