// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2018 Red Hat <contact@redhat.com>
 * Author: Adam C. Emerson <aemerson@redhat.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_COMMON_ASYNC_CONTEXT_POOL_H
#define CEPH_COMMON_ASYNC_CONTEXT_POOL_H

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "common/ceph_mutex.h"
#include "common/Thread.h"

#ifdef HAVE_SCHED
#include <sched.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

namespace ceph::async {
class io_context_pool {
  std::vector<std::thread> threadvec;
  boost::asio::io_context ioctx;
  std::optional<boost::asio::executor_work_guard<
		  boost::asio::io_context::executor_type>> guard;
  ceph::mutex m = make_mutex("ceph::io_context_pool::m");
  size_t cpu_set_size = 0;
  // cpu_set is only modified in set_cpu_affinity() when threadvec is empty
  // (enforced by assertion), ensuring no concurrent access from worker threads.
  // Worker threads only read these values in apply_cpu_affinity() during startup,
  // which happens after set_cpu_affinity() completes but before the thread starts work.
  cpu_set_t cpu_set;

  void cleanup() noexcept {
    guard = std::nullopt;
    for (auto& th : threadvec) {
      th.join();
    }
    threadvec.clear();
  }

  void apply_cpu_affinity() noexcept {
#ifdef HAVE_SCHED
    if (cpu_set_size > 0) {
      // Set CPU affinity for this thread
      // cpu_set_size is used to track that affinity is enabled,
      // but we always pass sizeof(cpu_set_t) to sched_setaffinity.
      // This is safe because cpu_set is only modified before threads start
      // (in set_cpu_affinity, which is always called before start()).
      if (sched_setaffinity(0, sizeof(cpu_set_t), &cpu_set) < 0) {
        // Log error to stderr since we don't have access to CephContext here
        // This is a non-fatal error - the thread will continue without affinity
        int err = errno;
        char err_buf[256];
        char msg_buf[512];
        
        // Use thread-safe strerror_r (handle both POSIX and GNU versions)
        #if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L && !defined(_GNU_SOURCE)
        // POSIX version: returns int, writes to buffer
        strerror_r(err, err_buf, sizeof(err_buf));
        const char* err_str = err_buf;
        #else
        // GNU version: returns char* (may point to static buffer or err_buf)
        const char* err_str = strerror_r(err, err_buf, sizeof(err_buf));
        #endif
        
        int len = snprintf(msg_buf, sizeof(msg_buf),
                          "io_context_pool: failed to set CPU affinity: %s (errno=%d)\n",
                          err_str, err);
        
        // Write error message to stderr
        // snprintf returns the number of chars that would be written (excluding null)
        // If truncated, len >= sizeof(msg_buf), so we write sizeof(msg_buf) - 1
        if (len > 0) {
          size_t write_len = ((size_t)len < sizeof(msg_buf)) ? len : sizeof(msg_buf) - 1;
          (void)write(STDERR_FILENO, msg_buf, write_len);
        }
      } else {
        // Yield to allow the scheduler to migrate this thread to the appropriate CPU
        sched_yield();
      }
    }
#endif
  }

public:
  io_context_pool() noexcept {
#ifdef HAVE_SCHED
    // Initialize cpu_set to a clean state for platforms with scheduler support
    CPU_ZERO(&cpu_set);
#endif
  }

  io_context_pool(std::int64_t threadcnt) noexcept {
    start(threadcnt);
  }
  template<std::invocable<> Init>
  io_context_pool(std::int64_t threadcnt, Init&& init) noexcept {
    start(threadcnt, std::move(init));
  }
  ~io_context_pool() {
    stop();
  }

#ifdef HAVE_SCHED
  void set_cpu_affinity(size_t size, const cpu_set_t *set) noexcept {
    auto l = std::scoped_lock(m);
    // This should only be called before start() or after stop()
    // to avoid race conditions with running threads
    ceph_assert(threadvec.empty());
    cpu_set_size = size;
    cpu_set = *set;
  }
#endif

  void start(std::int16_t threadcnt) noexcept {
    auto l = std::scoped_lock(m);
    if (threadvec.empty()) {
      guard.emplace(boost::asio::make_work_guard(ioctx));
      ioctx.restart();
      for (std::int16_t i = 0; i < threadcnt; ++i) {
	threadvec.emplace_back(make_named_thread("io_context_pool",
						 [this] {
						   apply_cpu_affinity();
						   ioctx.run();
						 }));
      }
    }
  }
  template<std::invocable<> Init>
  void start(std::int16_t threadcnt, Init&& init) noexcept {
    auto l = std::scoped_lock(m);
    if (threadvec.empty()) {
      guard.emplace(boost::asio::make_work_guard(ioctx));
      ioctx.restart();
      for (std::int16_t i = 0; i < threadcnt; ++i) {
	threadvec.emplace_back(make_named_thread("io_context_pool",
						 [this, init=std::move(init)] {
						   apply_cpu_affinity();
						   std::move(init)();
						   ioctx.run();
						 }));
      }
    }
  }
  void finish() noexcept {
    auto l = std::scoped_lock(m);
    if (!threadvec.empty()) {
      cleanup();
    }
  }
  void stop() noexcept {
    auto l = std::scoped_lock(m);
    if (!threadvec.empty()) {
      ioctx.stop();
      cleanup();
    }
  }

  boost::asio::io_context& get_io_context() {
    return ioctx;
  }
  operator boost::asio::io_context&() {
    return ioctx;
  }
  using executor_type = boost::asio::io_context::executor_type;
  boost::asio::io_context::executor_type get_executor() {
    return ioctx.get_executor();
  }
};
}

#endif // CEPH_COMMON_ASYNC_CONTEXT_POOL_H
