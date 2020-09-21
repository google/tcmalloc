// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Runs a simulation of malloc traffic across many threads, based on real-ish
// profiles.  Configurable levels of different kinds of usage across threads.

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <new>
#include <random>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/memory/memory.h"
#include "absl/random/bernoulli_distribution.h"
#include "absl/random/uniform_int_distribution.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/barrier.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/empirical.h"
#include "tcmalloc/testing/empirical_distributions.h"
#include "tcmalloc/testing/testutil.h"

ABSL_FLAG(std::string, profile, "",
          "Which source of profiled allocation to use for the base load. "
          "This can be beta, bravo, charlie, echo, merced, sierra, sigma, or "
          "uniform. It can also be a uint64_t N, which means allocate N-byte "
          "objects exclusively.");

ABSL_FLAG(uint64_t, threads, 1, "Number of parallel allocators");

ABSL_FLAG(uint64_t, bytes, 16ul << 30, "Total size of base heap");

ABSL_FLAG(uint64_t, transient_bytes, 0,
          "Additional size of data allocated at program start, then freed "
          "before running main benchmark");

ABSL_FLAG(std::string, transient_profile, "",
          "If --transient_profile has one of the valid values for --profile "
          "it has the same meaning. Otherwise must be the empty string, "
          "which means copy --profile.");

ABSL_FLAG(uint64_t, spike_bytes, 0,
          "Additional memory allocated periodically. "
          "Could model per-query memory or diurnal variation, etc.");

ABSL_FLAG(std::string, spike_profile, "",
          "If --spike_profile has one of the valid values for --profile it "
          "has the same meaning. Otherwise must be the empty string, which "
          "means copy --profile.");

ABSL_FLAG(absl::Duration, spike_rate, absl::Milliseconds(10),
          "1/QPS for spikes");

ABSL_FLAG(absl::Duration, spike_lifetime, absl::Milliseconds(30),
          "Processing time for spikes");

ABSL_FLAG(bool, spikes_exact, false,
          "should spike rates/lifetimes be exactly the average (instead of "
          "randomized?)");

ABSL_FLAG(bool, spikes_shared, false,
          "should spikes be split among threads equally (instead of on one "
          "chosen thread?)");

ABSL_FLAG(double, spike_locality, 0.90,
          "Probability a spike freed by the thread which created it");

ABSL_FLAG(int64_t, simulated_bytes_per_sec, 0,
          "If non-0, empirical driver will simulate tick of "
          "ReleaseMemoryToOS iteration each given number of bytes allocated");

ABSL_FLAG(bool, print_stats_to_file, true, "Write mallocz stats to a file");

ABSL_FLAG(int64_t, empirical_malloc_release_bytes_per_sec, 0,
          "Number of bytes to try to release from the page heap per second");

namespace tcmalloc {
namespace empirical {
namespace {

void *alloc(size_t s) { return ::operator new(s); }

const EmpiricalProfile *ParseProfileChoice(const absl::string_view flag) {
  std::map<absl::string_view, const EmpiricalProfile> choices = {
      {"beta", empirical_distributions::Beta()},
      {"bravo", empirical_distributions::Bravo()},
      {"charlie", empirical_distributions::Charlie()},
      {"echo", empirical_distributions::Echo()},
      {"merced", empirical_distributions::Merced()},
      {"sierra", empirical_distributions::Sierra()},
      {"sigma", empirical_distributions::Sigma()},
      {"uniform", empirical_distributions::Uniform()}};
  auto i = choices.find(flag);
  if (i == choices.end()) {
    uint64_t bytes;
    // TODO(ckennelly):  Integrate this with AbslParseFlag.
    CHECK_CONDITION(absl::SimpleAtoi(flag, &bytes));
    return new EmpiricalProfile({{bytes, 1, 1}});
  }
  return &i->second;
}

const EmpiricalProfile &Profile() {
  static const EmpiricalProfile *choice =
      ParseProfileChoice(absl::GetFlag(FLAGS_profile));
  return *choice;
}

const EmpiricalProfile *ParseProfileChoiceWithDefault(
    const absl::string_view flag) {
  return flag.empty() ? &Profile() : ParseProfileChoice(flag);
}

const EmpiricalProfile &SpikeProfile() {
  static const EmpiricalProfile *choice =
      ParseProfileChoiceWithDefault(absl::GetFlag(FLAGS_spike_profile));
  return *choice;
}

const EmpiricalProfile &TransientProfile() {
  static const EmpiricalProfile *choice =
      ParseProfileChoiceWithDefault(absl::GetFlag(FLAGS_transient_profile));
  return *choice;
}

class SequenceNumber {
 public:
  constexpr SequenceNumber() : value_(0) {}
  SequenceNumber(const SequenceNumber &) = delete;
  SequenceNumber &operator=(const SequenceNumber &) = delete;

  intptr_t GetNext() { return value_.fetch_add(1, std::memory_order_relaxed); }

 private:
  std::atomic<intptr_t> value_;
};

ABSL_CONST_INIT SequenceNumber seeds;
ABSL_CONST_INIT tcmalloc_internal::StatsCounter live_spikes;
ABSL_CONST_INIT tcmalloc_internal::StatsCounter spike_usage;
ABSL_CONST_INIT tcmalloc_internal::StatsCounter dead_spikes;
ABSL_CONST_INIT tcmalloc_internal::StatsCounter reps;

// This represents a sudden increase in usage (think of it as the allocation
// required to serve a query) which lives for a while, then dies.
class Spike {
 public:
  explicit Spike(size_t size)
      : data_(seeds.GetNext(), SpikeProfile(), size, alloc, sized_delete) {
    spike_usage.Add(data_.usage());
    live_spikes.Add(1);
  }

  ~Spike() {
    spike_usage.Add(-static_cast<ssize_t>(data_.usage()));
    dead_spikes.Add(1);
  }

  size_t bytes_allocated() const { return data_.total_bytes_allocated(); }

 private:
  EmpiricalData data_;
};

// There are basically three configurations of spikes, each of which
// generates an overall average of (lifetime/rate) live spikes of size
// <spike_bytes>; they do so in different ways (and with different
// numbers of Spike objects.)
//
// A) each thread independently generates full size spikes with random
// timing.  Each thread has an indepedent Poisson process; the Spike
// objects have size spike_bytes; each thread's interarrival is scaled
// down so the overall rate is spike_rate. (This is !FLAGS_spikes_exact &&
// !FLAGS_spikes_shared.)
//
// B) One thread generates a full size spike every <rate> in lockstep
// (the thread chosen at random.)  (This is FLAGS_spikes_exact &&
// !FLAGS_spikes_shared.) Spike objects have size spike_bytes, but the
// interarrival time is *not* scaled down (we just only have one
// thread waiting for the right time.) We do this to prevent regular
// patterns in who makes spikes.
//
// C) Each thread *NON*-independently generates small spikes with
// (random or exact) timing matching one each other, adding to a total
// of spike_bytes.  Spike objects have size spike_bytes / N, and
// interarrival is *not* scaled down.  (This is FLAGS_spikes_shared, exact or
// inexact.)

absl::Duration SpikeInterarrival() {
  // Remember, high scale, since this is an interarrival mean, decreases rate.
  static size_t scale =
      (absl::GetFlag(FLAGS_spikes_shared) || absl::GetFlag(FLAGS_spikes_exact))
          ? 1
          : absl::GetFlag(FLAGS_threads);
  static absl::Duration d = absl::GetFlag(FLAGS_spike_rate) * scale;
  return d;
}

absl::Duration SpikeAvgLifetime() {
  // This, thankfully, is treated the same in all configs.
  static absl::Duration d = absl::GetFlag(FLAGS_spike_lifetime);
  return d;
}

size_t SpikeSize() {
  static size_t scale =
      absl::GetFlag(FLAGS_spikes_shared) ? absl::GetFlag(FLAGS_threads) : 1;
  static size_t s = absl::GetFlag(FLAGS_spike_bytes) / scale;
  return s;
}

bool SpikesExact() {
  static bool b = absl::GetFlag(FLAGS_spikes_exact);
  return b;
}

bool SpikesShared() {
  static bool b = absl::GetFlag(FLAGS_spikes_shared);
  return b;
}

bool OneSpiker() {
  static bool b = SpikesExact() && !SpikesShared();
  return b;
}

absl::Duration Exp(absl::BitGen *random, absl::Duration mean) {
  return std::exponential_distribution<double>()(*random) * mean;
}

absl::Duration SpikeLifetime(absl::BitGen *random) {
  if (SpikesExact()) return SpikeAvgLifetime();
  return Exp(random, SpikeAvgLifetime());
}

// On the n-th call to this function from a given thread, returns the proper
// time for the n-th spike from that thread. If SpikesShared(), those times will
// be the same for any thread; otherwise, they're independent.
absl::Time SpikeTime() {
  // This case uses different logic.
  CHECK_CONDITION(!OneSpiker());
  // Don't allow skew in thread creation time.
  static absl::Time begin = absl::Now();
  thread_local size_t i = 0;
  i++;
  // First the easy case:
  if (SpikesExact()) return begin + i * SpikeInterarrival();

  // Have a thread local generator for interarrival times.  If we're shared,
  // they should all have the same seed (we still use local generators for
  // scalability.)
  thread_local absl::BitGen local_gen(absl::SeedSeq{[]() -> size_t {
    if (SpikesShared()) {
      static size_t globalseed = seeds.GetNext();
      return globalseed;
    }

    return seeds.GetNext();
  }()});

  thread_local absl::Time last = begin;
  last += Exp(&local_gen, SpikeInterarrival());
  return last;
}

class SimThread {
 public:
  SimThread(int n, absl::Barrier *startup,
            const std::vector<SimThread *> &siblings, size_t bytes,
            size_t transient)
      : n_(n),
        startup_(startup),
        siblings_(siblings),
        bytes_(bytes),
        transient_(transient),
        spike_is_local_(absl::GetFlag(FLAGS_spike_locality)) {
    if (n == 0) {
      auto nthreads = siblings.size();
      run_release_each_bytes_ =
          (absl::GetFlag(FLAGS_simulated_bytes_per_sec) + nthreads - 1) /
          nthreads;
    }
  }

  size_t total_bytes_allocated() {
    return load_bytes_allocated_.load(std::memory_order_relaxed) +
           spike_bytes_allocated_.load(std::memory_order_relaxed);
  }

  size_t usage() { return load_usage_.load(std::memory_order_relaxed); }

  void Run() {
    EmpiricalData load(n_, Profile(), bytes_, alloc, sized_delete);
    if (transient_ > 0) {
      auto transient = absl::make_unique<EmpiricalData>(
          n_, TransientProfile(), transient_, alloc, sized_delete);
      startup_->Block();
      transient.reset(nullptr);
    } else {
      startup_->Block();
    }
    {
      absl::base_internal::SpinLockHolder h(&lock_);
      next_spike_ = FirstSpike();
    }
    while (true) {
      // Every so often we need to do something else (i.e. spawn a spike) but I
      // don't want to go through the overhead of computing times every
      // iteration.  100 reps means something like 150usec, which is still
      // plenty good resolution, but very low overhead.
      static const size_t kBatch = 100;
      for (int i = 0; i < kBatch; ++i) {
        load.Next();
      }
      absl::Time t = absl::Now();
      reps.Add(kBatch);
      auto allocated = load.total_bytes_allocated();
      load_bytes_allocated_.store(allocated, std::memory_order_relaxed);

      if (run_release_each_bytes_ != 0 && allocated >= next_release_boundary_) {
        next_release_boundary_ += run_release_each_bytes_;
        MallocExtension::ReleaseMemoryToSystem(
            absl::GetFlag(FLAGS_empirical_malloc_release_bytes_per_sec));
      }

      load_usage_.store(load.usage(), std::memory_order_relaxed);
      if (KillSpikesCheckNew(t)) {
        MakeSpike(t, load.rng());
      }
    }
  }

 private:
  // returns true if we should make a new spike too!
  bool KillSpikesCheckNew(absl::Time t) {
    std::vector<Spike *> dead;
    bool ready_for_new;
    {
      absl::base_internal::SpinLockHolder h(&lock_);
      ready_for_new = t >= next_spike_;

      auto it = to_kill_.begin();
      for (; it != to_kill_.end() && t < it->first; ++it) {
        dead.insert(dead.end(), it->second.begin(), it->second.end());
      }
      to_kill_.erase(to_kill_.begin(), it);
    }

    for (auto *d : dead) {
      delete d;
    }

    return ready_for_new;
  }

  absl::Time FirstSpike() {
    if (SpikeSize() == 0) return absl::InfiniteFuture();
    if (OneSpiker()) {
      // Only one thread should make spikes; we arbitrarily start with 0.
      if (n_ != 0) return absl::InfiniteFuture();
      return absl::Now() + SpikeInterarrival();
    }

    return SpikeTime();
  }

  void MakeSpike(absl::Time t, absl::BitGen *random) {
    // First, compute next spike times, in different ways depending on our
    // scenario.
    if (OneSpiker()) {
      // We make one more spike, but not necessarily on *us*.
      absl::Duration d = SpikeInterarrival();
      absl::Time when;
      {
        absl::base_internal::SpinLockHolder h(&lock_);
        when = next_spike_ + d;
        next_spike_ = absl::InfiniteFuture();
      }
      size_t i = absl::uniform_int_distribution<size_t>(
          0, siblings_.size() - 1)(*random);
      SimThread *who = siblings_[i];
      absl::base_internal::SpinLockHolder h(&who->lock_);
      who->next_spike_ = when;
    } else {
      absl::Time when = SpikeTime();
      absl::base_internal::SpinLockHolder h(&lock_);
      next_spike_ = when;
    }

    Spike *s = new Spike(SpikeSize());
    spike_bytes_allocated_.fetch_add(s->bytes_allocated());
    absl::Duration life = SpikeLifetime(random);
    absl::Time death = absl::Now() + life;
    SimThread *killer = this;
    // This slightly overselects ourselves, since we might be the random
    // sibling, but that's minor.
    if (!spike_is_local_(*random)) {
      size_t i = absl::uniform_int_distribution<size_t>(
          0, siblings_.size() - 1)(*random);
      killer = siblings_[i];
    }

    absl::base_internal::SpinLockHolder h(&killer->lock_);
    killer->to_kill_[death].push_back(s);
  }

  size_t n_;
  absl::Time next_spike_ ABSL_GUARDED_BY(lock_);
  absl::Barrier *startup_;
  const std::vector<SimThread *> &siblings_;
  size_t bytes_, transient_;
  absl::bernoulli_distribution spike_is_local_;
  std::atomic<size_t> load_bytes_allocated_{0};
  std::atomic<size_t> spike_bytes_allocated_{0};
  std::atomic<size_t> load_usage_{0};
  absl::base_internal::SpinLock lock_;
  std::map<absl::Time, std::vector<Spike *>> to_kill_ ABSL_GUARDED_BY(lock_);
  size_t run_release_each_bytes_{};
  size_t next_release_boundary_{};
};

void SetContents(std::string filename, std::string contents) {

  int fd = tcmalloc_internal::signal_safe_open(filename.c_str(), O_WRONLY);
  CHECK_CONDITION(fd >= 0);
  CHECK_CONDITION(tcmalloc_internal::signal_safe_write(
                      fd, contents.data(), contents.size(), nullptr) ==
                  contents.size());
  tcmalloc_internal::signal_safe_close(fd);
}

void PrintStatsToFile() {
  static int n = 0;
  static const size_t pid = getpid();
  std::string stats = MallocExtension::GetStats();
  std::string fname = absl::StrCat("mallocz.", pid, ".", n);
  SetContents(std::move(fname), std::move(stats));
  n++;
}

using BinF = size_t;
using EngF = size_t;

size_t GetProp(absl::string_view name) {
  absl::optional<size_t> x = MallocExtension::GetNumericProperty(name);
  CHECK_CONDITION(x.has_value());
  return *x;
}

}  // namespace

void RunSim() {
  // Initialize the profile now before startup so we get early errors.
  Profile();
  SpikeProfile();
  TransientProfile();

  const size_t nthreads = absl::GetFlag(FLAGS_threads);
  const size_t per_thread_size = absl::GetFlag(FLAGS_bytes) / nthreads;
  const size_t per_thread_transient =
      absl::GetFlag(FLAGS_transient_bytes) / nthreads;
  const bool print_stats = absl::GetFlag(FLAGS_print_stats_to_file);

  absl::Barrier b(nthreads + 1);
  std::vector<SimThread *> state(nthreads, nullptr);
  std::vector<std::thread> threads;
  threads.reserve(nthreads);
  for (size_t i = 0; i < nthreads; ++i) {
    state[i] =
        new SimThread(i, &b, state, per_thread_size, per_thread_transient);
    threads.push_back(std::thread(&SimThread::Run, state[i]));
  }
  b.Block();
  absl::Time last_mallocz = absl::InfinitePast();
  absl::Time last = absl::InfinitePast();
  size_t last_spikes_completed = 0;
  size_t last_bytes = 0;
  absl::Time start = absl::Now();
  while (true) {
    absl::SleepFor(absl::Milliseconds(750));
    const absl::Time t = absl::Now();
    const absl::Duration dur = t - last;
    const absl::Duration life = t - start;
    if (print_stats && t - last_mallocz > absl::Seconds(10)) {
      PrintStatsToFile();
      last_mallocz = t;
    }
    size_t bytes = 0;
    size_t usage = 0;
    size_t bot = std::numeric_limits<size_t>::max(),
           top = std::numeric_limits<size_t>::min();
    for (const auto &s : state) {
      bytes += s->total_bytes_allocated();
      size_t u = s->usage();
      bot = std::min(u, bot);
      top = std::max(u, top);
      usage += u;
    }
    absl::PrintF("Total: %fiB (%fiB / %fiB)\n", BinF(usage), BinF(bot),
                 BinF(top));
    const size_t live = live_spikes.value() - dead_spikes.value();
    const double dur_s = absl::FDivDuration(dur, absl::Seconds(1));
    const double life_s = absl::FDivDuration(life, absl::Seconds(1));
    const size_t spikes_completed = dead_spikes.value();
    const double cur_spike_rate =
        (spikes_completed - last_spikes_completed) / dur_s;
    const double life_spike_rate = spikes_completed / life_s;
    const double byte_rate = (bytes - last_bytes) / dur_s;
    absl::PrintF(
        "Time: %zu live spikes (%fiB), %f spikes / s recently (%f / s "
        "lifetime), %fiB allocated / s\n",
        live, BinF(spike_usage.value()), EngF(cur_spike_rate),
        EngF(life_spike_rate), BinF(byte_rate));

    const size_t in_use = GetProp("generic.current_allocated_bytes");
    const size_t local = GetProp("tcmalloc.local_bytes");
    const size_t pageheap = GetProp("tcmalloc.pageheap_free_bytes");
    const size_t released = GetProp("tcmalloc.pageheap_unmapped_bytes");
    const size_t waste = GetProp("tcmalloc.external_fragmentation_bytes");
    const size_t central = waste - local - pageheap;
    absl::PrintF("Space: %8f , %8f =  %8f + %8f + %8f | %8f\n", BinF(in_use),
                 BinF(waste), BinF(local), BinF(central), BinF(pageheap),
                 BinF(released));
    last = t;
    last_bytes = bytes;
    last_spikes_completed = spikes_completed;
  }

  for (auto &t : threads) {
    t.join();
  }
}

}  // namespace empirical
}  // namespace tcmalloc

int main(int argc, char *argv[]) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  absl::ParseCommandLine(argc, argv);
  tcmalloc::empirical::RunSim();

  return 0;
}
