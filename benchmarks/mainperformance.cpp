#include "peak_pulse/perf_statistics.hpp"
#include "peak_pulse/ring_buffer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/utsname.h>
#endif

#ifndef PEAK_PULSE_BUILD_TYPE
#define PEAK_PULSE_BUILD_TYPE "unknown"
#endif
#ifndef PEAK_PULSE_BUILD_FLAGS
#define PEAK_PULSE_BUILD_FLAGS "not recorded"
#endif
#ifndef PEAK_PULSE_GIT_COMMIT
#define PEAK_PULSE_GIT_COMMIT "unavailable"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

struct Options final {
  std::string scenario{"all"};
  std::size_t producers{4};
  std::size_t consumers{4};
  std::size_t capacity{1024};
  std::size_t items{100'000};
  double duration_seconds{1.0};
  double warmup_seconds{0.25};
  std::size_t repetitions{5};
  bool pin{false};
  std::string json_path;
  std::string csv_path;
  std::uint64_t seed{1};
};

[[nodiscard]] constexpr std::uint64_t xor_ids(std::uint64_t count) noexcept {
  switch (count & 3U) {
    case 0: return 0;
    case 1: return count - 1;
    case 2: return 1;
    default: return count;
  }
}

struct Workload final {
  std::string scenario;
  std::size_t producers;
  std::size_t consumers;
};

struct PerfItem final {
  std::uint64_t id;
  std::int64_t pushed_at_ns;
};

static_assert(peak_pulse::NothrowMoveConstructible<PerfItem>);

struct alignas(64) ThreadCounter final {
  std::uint64_t value{0};
};

struct alignas(64) AtomicHistogram final {
  static constexpr std::size_t bucket_count =
      peak_pulse::perf::LogHistogram::bucket_count;
  std::unique_ptr<std::atomic<std::uint64_t>[]> buckets{
      std::make_unique<std::atomic<std::uint64_t>[]>(bucket_count)};
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::uint64_t> maximum{0};

  AtomicHistogram() = default;
  AtomicHistogram(const AtomicHistogram&) = delete;
  AtomicHistogram& operator=(const AtomicHistogram&) = delete;

  AtomicHistogram(AtomicHistogram&& other) noexcept
      : buckets(std::move(other.buckets)),
        count(other.count.load(std::memory_order_relaxed)),
        maximum(other.maximum.load(std::memory_order_relaxed)) {
  }

  AtomicHistogram& operator=(AtomicHistogram&&) = delete;

  void record(std::uint64_t value) noexcept {
    const std::size_t bucket = peak_pulse::perf::LogHistogram::bucket_for(value);
    buckets[bucket].fetch_add(1, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t current = maximum.load(std::memory_order_relaxed);
    while (current < value &&
           !maximum.compare_exchange_weak(current, value,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    }
  }

  [[nodiscard]] std::uint64_t percentile(double fraction) const noexcept {
    const std::uint64_t sample_count = count.load(std::memory_order_relaxed);
    if (sample_count == 0) {
      return 0;
    }
    const auto rank = static_cast<std::uint64_t>(
        std::ceil(fraction * static_cast<double>(sample_count)));
    std::uint64_t cumulative = 0;
    for (std::size_t bucket = 0; bucket < bucket_count; ++bucket) {
      cumulative += buckets[bucket].load(std::memory_order_relaxed);
      if (cumulative >= std::max<std::uint64_t>(1, rank)) {
        if (bucket == 0) {
          return 0;
        }
        if (bucket == bucket_count - 1) {
          return std::numeric_limits<std::uint64_t>::max();
        }
        const std::size_t logarithmic_bucket = bucket - 1;
        const unsigned exponent = static_cast<unsigned>(
            logarithmic_bucket /
            peak_pulse::perf::LogHistogram::sub_buckets);
        const std::uint64_t sub_bucket = static_cast<std::uint64_t>(
            logarithmic_bucket %
            peak_pulse::perf::LogHistogram::sub_buckets);
        const std::uint64_t base = std::uint64_t{1} << exponent;
        if (exponent < 7U) {
          return base + (sub_bucket >> (7U - exponent));
        }
        const std::uint64_t width = std::uint64_t{1} << (exponent - 7U);
        return base + (sub_bucket + 1U) * width - 1U;
      }
    }
    return maximum.load(std::memory_order_relaxed);
  }
};

struct RunResult final {
  double seconds{0.0};
  std::uint64_t delivered{0};
  std::uint64_t checksum{0};
  bool valid{false};
  std::string error;
  AtomicHistogram latency;
};

struct Summary final {
  double median{0.0};
  double mean{0.0};
  double standard_deviation{0.0};
  double minimum{0.0};
  double maximum{0.0};
  double p95{0.0};
  peak_pulse::perf::ConfidenceInterval median_ci{0.0, 0.0};
};

struct Record final {
  std::string scenario;
  std::string queue;
  std::size_t producers;
  std::size_t consumers;
  std::size_t capacity;
  double median_items_per_second;
  double speedup_vs_mutex;
  Summary throughput;
  std::vector<double> repetitions;
  std::array<double, 4> offered_loads{};
  std::array<std::array<std::uint64_t, 6>, 4> latency_percentiles{};
  std::array<std::uint64_t, 4> latency_maximum{};
};

void print_usage(std::ostream& output) {
  output
      << "Usage: peak_pulse_perf [options]\n"
      << "  --scenario {spsc,mpsc,mpmc,all}  default: all\n"
      << "  --producers N --consumers N --capacity N --items N\n"
      << "  --duration-sec S --warmup-sec S --repetitions R\n"
      << "  --pin --json out.json --csv out.csv --seed N\n";
}

template <typename Integer>
[[nodiscard]] bool parse_integer(std::string_view text, Integer& output) {
  if (text.empty()) {
    return false;
  }
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, output);
  return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_double(std::string_view text, double& output) {
  std::string copy(text);
  char* end = nullptr;
  output = std::strtod(copy.c_str(), &end);
  return end == copy.c_str() + copy.size() && std::isfinite(output);
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--pin") {
      options.pin = true;
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      print_usage(std::cout);
      std::exit(0);
    }
    if (index + 1 >= argc) {
      std::cerr << "Missing value for " << argument << '\n';
      return false;
    }
    const std::string_view value(argv[++index]);
    if (argument == "--scenario") {
      options.scenario = value;
    } else if (argument == "--producers") {
      if (!parse_integer(value, options.producers)) return false;
    } else if (argument == "--consumers") {
      if (!parse_integer(value, options.consumers)) return false;
    } else if (argument == "--capacity") {
      if (!parse_integer(value, options.capacity)) return false;
    } else if (argument == "--items") {
      if (!parse_integer(value, options.items)) return false;
    } else if (argument == "--duration-sec") {
      if (!parse_double(value, options.duration_seconds)) return false;
    } else if (argument == "--warmup-sec") {
      if (!parse_double(value, options.warmup_seconds)) return false;
    } else if (argument == "--repetitions") {
      if (!parse_integer(value, options.repetitions)) return false;
    } else if (argument == "--json") {
      options.json_path = value;
    } else if (argument == "--csv") {
      options.csv_path = value;
    } else if (argument == "--seed") {
      if (!parse_integer(value, options.seed)) return false;
    } else {
      std::cerr << "Unknown option: " << argument << '\n';
      return false;
    }
  }

  if (options.scenario != "spsc" && options.scenario != "mpsc" &&
      options.scenario != "mpmc" && options.scenario != "all") {
    std::cerr << "Invalid scenario: " << options.scenario << '\n';
    return false;
  }
  if (options.producers == 0 || options.consumers == 0 ||
      options.items == 0 || options.repetitions == 0 ||
      options.duration_seconds <= 0.0 || options.warmup_seconds < 0.0 ||
      options.producers > 256 || options.consumers > 256 ||
      options.items > std::numeric_limits<std::size_t>::max() /
                          options.producers) {
    std::cerr << "Counts must be positive, duration positive, warmup nonnegative, "
                 "and thread counts <= 256\n";
    return false;
  }
  if (options.capacity < 2 ||
      (options.capacity & (options.capacity - 1)) != 0 ||
      options.capacity > 65536) {
    std::cerr << "Capacity must be a power of two in [2, 65536]\n";
    return false;
  }
  if (options.scenario == "spsc" &&
      (options.producers != 1 || options.consumers != 1)) {
    std::cerr << "spsc requires --producers 1 --consumers 1\n";
    return false;
  }
  if (options.scenario == "mpsc" && options.consumers != 1) {
    std::cerr << "mpsc requires --consumers 1\n";
    return false;
  }
  if ((options.scenario == "mpsc" || options.scenario == "mpmc" ||
       options.scenario == "all") &&
      options.producers < 2) {
    std::cerr << "mpsc, mpmc, and all require at least two producers\n";
    return false;
  }
  if ((options.scenario == "mpmc" || options.scenario == "all") &&
      options.consumers < 2) {
    std::cerr << "mpmc and all require at least two consumers\n";
    return false;
  }
  return true;
}

[[nodiscard]] std::string json_escape(std::string_view input) {
  std::string output;
  for (const char character : input) {
    switch (character) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output += character; break;
    }
  }
  return output;
}

[[nodiscard]] std::string timestamp_utc() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  std::ostringstream stream;
  stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return stream.str();
}

[[nodiscard]] std::string cpu_model() {
#if defined(_WIN32)
  const char* identifier = std::getenv("PROCESSOR_IDENTIFIER");
  return identifier == nullptr ? "unknown" : identifier;
#elif defined(__linux__)
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    if (line.rfind("model name", 0) == 0 || line.rfind("Hardware", 0) == 0) {
      const auto separator = line.find(':');
      if (separator != std::string::npos) {
        return line.substr(separator + 2);
      }
    }
  }
  return "unknown";
#else
  return "unknown";
#endif
}

[[nodiscard]] std::string operating_system() {
#if defined(_WIN32)
  return "Windows";
#elif defined(__linux__)
  struct utsname name {};
  if (uname(&name) == 0) {
    return std::string(name.sysname) + " " + name.release;
  }
  return "Linux";
#elif defined(__APPLE__)
  return "macOS";
#else
  return "unknown";
#endif
}

[[nodiscard]] bool pin_current_thread(std::size_t worker_index) noexcept {
#if defined(_WIN32)
  constexpr std::size_t mask_bits = sizeof(DWORD_PTR) * 8;
  const DWORD_PTR mask = DWORD_PTR{1} << (worker_index % mask_bits);
  return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  const int cpu = static_cast<int>(worker_index %
                                  static_cast<std::size_t>(CPU_SETSIZE));
  CPU_SET(cpu, &cpus);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus) == 0;
#else
  static_cast<void>(worker_index);
  return false;
#endif
}

void warn_if_invalid_build(const Options& options) {
  bool invalid_build = false;
#if !defined(NDEBUG)
  std::cerr << "WARNING: Debug build detected; benchmark numbers are invalid.\n";
  invalid_build = true;
#endif
#if defined(PEAK_PULSE_ASAN_ENABLED) || defined(PEAK_PULSE_UBSAN_ENABLED) || \
    defined(PEAK_PULSE_TSAN_ENABLED)
  std::cerr << "WARNING: Sanitizer build detected; benchmark numbers are invalid.\n";
  invalid_build = true;
#endif
  if (invalid_build) {
    std::cerr << "WARNING: Rebuild with the release preset for performance results.\n";
  }
  if (options.pin) {
#if !defined(_WIN32) && !defined(__linux__)
    std::cerr << "WARNING: Thread pinning is a no-op on this platform.\n";
#endif
  }
}

class MutexDeque final {
 public:
  [[nodiscard]] bool try_push(PerfItem&& item) {
    const std::lock_guard<std::mutex> lock(mutex_);
    deque_.push_back(std::move(item));
    return true;
  }

  [[nodiscard]] std::optional<PerfItem> try_pop() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (deque_.empty()) {
      return std::nullopt;
    }
    PerfItem item = std::move(deque_.front());
    deque_.pop_front();
    return item;
  }

  [[nodiscard]] bool empty() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return deque_.empty();
  }

 private:
  std::mutex mutex_;
  std::deque<PerfItem> deque_;
};

template <typename Queue>
[[nodiscard]] bool queue_empty(Queue& queue) {
  if constexpr (requires { queue.empty_approx(); }) {
    return queue.empty_approx();
  } else {
    return queue.empty();
  }
}

struct RunConfig final {
  std::size_t producers;
  std::size_t consumers;
  std::size_t items_each;
  double timed_producer_seconds;
  double offered_items_per_second;
  bool pin;
};

template <typename Queue>
[[nodiscard]] RunResult run_workload(Queue& queue, const RunConfig& config) {
  RunResult result;
  const bool timed_production = config.items_each == 0;
  const std::size_t expected_items =
      timed_production ? 0 : config.producers * config.items_each;
  auto seen = expected_items == 0
                  ? nullptr
                  : std::make_unique<std::atomic<std::uint8_t>[]>(expected_items);
  for (std::size_t index = 0; index < expected_items; ++index) {
    seen[index].store(0, std::memory_order_relaxed);
  }
  auto producer_counts =
      std::make_unique<ThreadCounter[]>(config.producers);
  auto consumer_counts =
      std::make_unique<ThreadCounter[]>(config.consumers);
  std::atomic<std::size_t> producers_done{0};
  std::atomic<std::uint64_t> checksum{0};
  std::atomic<std::uint64_t> delivered{0};
  std::atomic<bool> invalid{false};
  std::atomic<std::uint64_t> sequence{0};
  std::barrier start_gate(
      static_cast<std::ptrdiff_t>(config.producers + config.consumers + 1));
  auto epoch = Clock::time_point{};
  std::vector<std::thread> threads;
  threads.reserve(config.producers + config.consumers);
  std::atomic<bool> pin_failed{false};

  for (std::size_t producer = 0; producer < config.producers; ++producer) {
    threads.emplace_back([&, producer] {
      if (config.pin && !pin_current_thread(producer)) {
        pin_failed.store(true, std::memory_order_relaxed);
      }
      start_gate.arrive_and_wait();
      const auto stop_at =
          epoch + std::chrono::duration_cast<Clock::duration>(
                      std::chrono::duration<double>(
                          config.timed_producer_seconds));
      auto next_due = epoch;
      const double interval =
          config.offered_items_per_second > 0.0
              ? static_cast<double>(config.producers) /
                    config.offered_items_per_second
              : 0.0;
      for (std::size_t local = 0;
           timed_production ? Clock::now() < stop_at
                            : local < config.items_each;
           ++local) {
        if (interval > 0.0) {
          next_due += std::chrono::duration_cast<Clock::duration>(
              std::chrono::duration<double>(interval));
          std::this_thread::sleep_until(next_due);
        }
        const std::uint64_t id =
            timed_production
                ? sequence.fetch_add(1, std::memory_order_relaxed)
                : static_cast<std::uint64_t>(producer * config.items_each +
                                             local);
        const auto intent = Clock::now();
        const auto intent_ns =
            config.offered_items_per_second > 0.0
                ? std::chrono::duration_cast<Nanoseconds>(
                      intent.time_since_epoch())
                      .count()
                : 0;
        PerfItem item{id, intent_ns};
        while (!queue.try_push(std::move(item))) {
          std::this_thread::yield();
        }
        ++producer_counts[producer].value;
      }
      producers_done.fetch_add(1, std::memory_order_release);
    });
  }

  for (std::size_t consumer = 0; consumer < config.consumers; ++consumer) {
    threads.emplace_back([&, consumer] {
      const std::size_t worker_index = config.producers + consumer;
      if (config.pin && !pin_current_thread(worker_index)) {
        pin_failed.store(true, std::memory_order_relaxed);
      }
      start_gate.arrive_and_wait();
      for (;;) {
        auto item = queue.try_pop();
        if (item.has_value()) {
          ++consumer_counts[consumer].value;
          if (!timed_production) {
            if (item->id >= expected_items ||
                seen[static_cast<std::size_t>(item->id)].fetch_add(
                    1, std::memory_order_relaxed) != 0) {
              invalid.store(true, std::memory_order_relaxed);
            }
          }
          if (item->pushed_at_ns != 0) {
            const auto now_ns = std::chrono::duration_cast<Nanoseconds>(
                                    Clock::now().time_since_epoch())
                                    .count();
            const auto latency = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, now_ns - item->pushed_at_ns));
            result.latency.record(latency);
          }
          checksum.fetch_xor(item->id, std::memory_order_relaxed);
          delivered.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (producers_done.load(std::memory_order_acquire) ==
                config.producers &&
            queue_empty(queue)) {
          break;
        }
        std::this_thread::yield();
      }
    });
  }

  epoch = Clock::now();
  const auto begin = epoch;
  start_gate.arrive_and_wait();
  for (std::thread& thread : threads) {
    thread.join();
  }
  const auto end = Clock::now();
  result.seconds = std::chrono::duration<double>(end - begin).count();
  result.delivered = delivered.load(std::memory_order_relaxed);
  result.checksum = checksum.load(std::memory_order_relaxed);
  std::uint64_t producer_total = 0;
  std::uint64_t consumer_total = 0;
  for (std::size_t index = 0; index < config.producers; ++index) {
    producer_total += producer_counts[index].value;
  }
  for (std::size_t index = 0; index < config.consumers; ++index) {
    consumer_total += consumer_counts[index].value;
  }
  result.valid = !invalid.load(std::memory_order_relaxed) &&
                 producers_done.load(std::memory_order_relaxed) ==
                     config.producers &&
                 producer_total == result.delivered &&
                 consumer_total == result.delivered &&
                 queue_empty(queue);
  if (!timed_production) {
    result.valid = result.valid && result.delivered == expected_items;
    for (std::size_t index = 0; index < expected_items; ++index) {
      result.valid =
          result.valid && seen[index].load(std::memory_order_relaxed) == 1;
    }
    result.valid =
        result.valid && result.checksum == xor_ids(expected_items);
  } else {
    const std::uint64_t expected_items_timed =
        sequence.load(std::memory_order_relaxed);
    result.valid = result.valid &&
                   result.delivered == expected_items_timed &&
                   result.checksum == xor_ids(expected_items_timed);
  }
  if (pin_failed.load(std::memory_order_relaxed)) {
    std::cerr << "WARNING: At least one thread could not be pinned.\n";
  }
  if (!result.valid) {
    result.error = "delivery validation failed";
  }
  return result;
}

template <std::size_t Capacity, typename QueueFactory>
[[nodiscard]] RunResult run_queue(const RunConfig& config,
                                  QueueFactory&& factory) {
  auto queue = factory.template operator()<Capacity>();
  return run_workload(*queue, config);
}

template <std::size_t Capacity>
[[nodiscard]] RunResult run_ring(const RunConfig& config) {
  peak_pulse::MpmcRingBuffer<PerfItem, Capacity> queue;
  return run_workload(queue, config);
}

[[nodiscard]] Summary summarize(const std::vector<double>& values,
                                std::uint64_t seed) {
  Summary summary;
  const auto med = peak_pulse::perf::median(values);
  const auto p95 = peak_pulse::perf::percentile(values, 0.95);
  if (!med || !p95 || values.empty()) {
    return summary;
  }
  summary.median = *med;
  summary.p95 = *p95;
  summary.minimum = *std::min_element(values.begin(), values.end());
  summary.maximum = *std::max_element(values.begin(), values.end());
  summary.mean =
      std::accumulate(values.begin(), values.end(), 0.0) /
      static_cast<double>(values.size());
  double squares = 0.0;
  for (const double value : values) {
    const double delta = value - summary.mean;
    squares += delta * delta;
  }
  summary.standard_deviation =
      std::sqrt(squares / static_cast<double>(values.size()));
  const auto interval =
      peak_pulse::perf::bootstrap_median_ci(values, 2000, seed);
  if (interval) {
    summary.median_ci = *interval;
  }
  return summary;
}

[[nodiscard]] std::vector<Workload> workloads_for(const Options& options) {
  std::vector<Workload> workloads;
  if (options.scenario == "spsc" || options.scenario == "all") {
    workloads.push_back({"spsc", 1, 1});
  }
  if (options.scenario == "mpsc" || options.scenario == "all") {
    workloads.push_back({"mpsc", options.producers, 1});
  }
  if (options.scenario == "mpmc" || options.scenario == "all") {
    workloads.push_back({"mpmc", options.producers, options.consumers});
  }
  return workloads;
}

template <typename Function>
decltype(auto) dispatch_capacity(std::size_t capacity, Function&& function) {
  switch (capacity) {
    case 2: return function.template operator()<2>();
    case 4: return function.template operator()<4>();
    case 8: return function.template operator()<8>();
    case 16: return function.template operator()<16>();
    case 32: return function.template operator()<32>();
    case 64: return function.template operator()<64>();
    case 128: return function.template operator()<128>();
    case 256: return function.template operator()<256>();
    case 512: return function.template operator()<512>();
    case 1024: return function.template operator()<1024>();
    case 2048: return function.template operator()<2048>();
    case 4096: return function.template operator()<4096>();
    case 8192: return function.template operator()<8192>();
    case 16384: return function.template operator()<16384>();
    case 32768: return function.template operator()<32768>();
    case 65536: return function.template operator()<65536>();
    default: std::abort();
  }
}

[[nodiscard]] RunResult run_mutex(const RunConfig& config) {
  MutexDeque queue;
  return run_workload(queue, config);
}

[[nodiscard]] bool run_latency(Record& record, const Workload& workload,
                               const Options& options, double max_throughput,
                               bool ring_buffer) {
  constexpr std::array<double, 4> load_fractions{0.25, 0.50, 0.75, 0.90};
  constexpr std::array<double, 6> percentiles{0.50, 0.90, 0.99,
                                             0.999, 0.9999, 1.0};
  const std::size_t latency_items_each =
      std::max<std::size_t>(1000, options.items / 10);
  for (std::size_t index = 0; index < load_fractions.size(); ++index) {
    const double target = max_throughput * load_fractions[index];
    record.offered_loads[index] = target;
    const std::size_t capacity = options.capacity;
    auto execute = [&]<std::size_t SelectedCapacity>() -> RunResult {
      RunConfig config{workload.producers, workload.consumers, latency_items_each,
                      0.0, target, options.pin};
      if (ring_buffer) {
        return run_ring<SelectedCapacity>(config);
      }
      return run_mutex(config);
    };
    RunResult run = dispatch_capacity(capacity, execute);
    if (!run.valid) {
      std::cerr << "INVALID latency run (" << record.queue << ", "
                << workload.scenario << "): " << run.error << '\n';
      return false;
    }
    for (std::size_t percentile_index = 0;
         percentile_index < percentiles.size(); ++percentile_index) {
      record.latency_percentiles[index][percentile_index] =
          run.latency.percentile(percentiles[percentile_index]);
    }
    record.latency_maximum[index] =
        run.latency.maximum.load(std::memory_order_relaxed);
  }
  return true;
}

[[nodiscard]] std::string compiler_description() {
#if defined(_MSC_VER)
  return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
  return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("GCC ") + __VERSION__;
#else
  return "unknown";
#endif
}

void write_json(const std::string& path, const std::vector<Record>& records,
                const Options& options) {
  std::ofstream output(path);
  if (!output) {
    std::cerr << "Cannot open JSON output: " << path << '\n';
    std::exit(2);
  }
  output << std::setprecision(17);
  output << "{\n  \"schema_version\": 1,\n"
         << "  \"environment\": {\n"
         << "    \"compiler\": \"" << json_escape(compiler_description())
         << "\",\n"
         << "    \"build_type\": \"" << json_escape(PEAK_PULSE_BUILD_TYPE)
         << "\",\n"
         << "    \"flags\": \"" << json_escape(PEAK_PULSE_BUILD_FLAGS)
         << "\",\n"
         << "    \"cpu_model\": \"" << json_escape(cpu_model()) << "\",\n"
         << "    \"logical_cores\": " << std::thread::hardware_concurrency()
         << ",\n"
         << "    \"os\": \"" << json_escape(operating_system()) << "\",\n"
         << "    \"git_commit\": \"" << json_escape(PEAK_PULSE_GIT_COMMIT)
         << "\",\n"
         << "    \"timestamp_utc\": \"" << timestamp_utc() << "\",\n"
         << "    \"seed\": " << options.seed << "\n"
         << "  },\n"
         << "  \"configuration\": {\"scenario\":\""
         << json_escape(options.scenario) << "\",\"producers\":"
         << options.producers << ",\"consumers\":" << options.consumers
         << ",\"capacity\":" << options.capacity
         << ",\"items_per_producer\":" << options.items
         << ",\"latency_items_per_producer\":"
         << std::max<std::size_t>(1000, options.items / 10)
         << ",\"duration_sec\":" << options.duration_seconds
         << ",\"warmup_sec\":" << options.warmup_seconds
         << ",\"repetitions\":" << options.repetitions
         << ",\"pinned\":" << (options.pin ? "true" : "false")
         << "},\n  \"results\": [\n";

  for (std::size_t index = 0; index < records.size(); ++index) {
    const Record& record = records[index];
    const Summary& summary = record.throughput;
    output << "    {\"scenario\":\"" << record.scenario << "\",\"queue\":\""
           << record.queue << "\",\"producers\":" << record.producers
           << ",\"consumers\":" << record.consumers
           << ",\"capacity\":" << record.capacity
           << ",\"items_per_second_median\":"
           << record.median_items_per_second << ",\"speedup_vs_mutex\":"
           << record.speedup_vs_mutex << ",\"mean\":"
           << summary.mean << ",\"stddev\":" << summary.standard_deviation
           << ",\"min\":" << summary.minimum << ",\"max\":"
           << summary.maximum << ",\"p95\":" << summary.p95
           << ",\"median_ci95\":[" << summary.median_ci.lower << ','
           << summary.median_ci.upper << "],\"repetitions\":[";
    for (std::size_t repetition = 0;
         repetition < record.repetitions.size(); ++repetition) {
      if (repetition != 0) output << ',';
      output << record.repetitions[repetition];
    }
    output << "],\"latency\":[";
    for (std::size_t load = 0; load < record.offered_loads.size(); ++load) {
      if (load != 0) output << ',';
      output << "{\"offered_items_per_second\":" << record.offered_loads[load]
             << ",\"p50_ns\":" << record.latency_percentiles[load][0]
             << ",\"p90_ns\":" << record.latency_percentiles[load][1]
             << ",\"p99_ns\":" << record.latency_percentiles[load][2]
             << ",\"p99_9_ns\":" << record.latency_percentiles[load][3]
             << ",\"p99_99_ns\":" << record.latency_percentiles[load][4]
             << ",\"max_ns\":" << record.latency_maximum[load] << '}';
    }
    output << "]}" << (index + 1 == records.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  if (!output) {
    std::cerr << "Failed while writing JSON output: " << path << '\n';
    std::exit(2);
  }
}

void write_csv(const std::string& path, const std::vector<Record>& records) {
  constexpr std::array<double, 4> load_fractions{0.25, 0.50, 0.75, 0.90};
  std::ofstream output(path);
  if (!output) {
    std::cerr << "Cannot open CSV output: " << path << '\n';
    std::exit(2);
  }
  output << "schema_version,scenario,queue,producers,consumers,capacity,"
            "median_items_per_second,speedup_vs_mutex,mean,stddev,min,max,p95,ci95_lower,"
            "ci95_upper,load_fraction,offered_items_per_second,p50_ns,p90_ns,"
            "p99_ns,p99_9_ns,p99_99_ns,max_latency_ns\n";
  for (const Record& record : records) {
    for (std::size_t load = 0; load < record.offered_loads.size(); ++load) {
      output << "1," << record.scenario << ',' << record.queue << ','
             << record.producers << ',' << record.consumers << ','
             << record.capacity << ',' << record.median_items_per_second << ','
             << record.speedup_vs_mutex << ',' << record.throughput.mean << ','
             << record.throughput.standard_deviation << ','
             << record.throughput.minimum << ',' << record.throughput.maximum
             << ',' << record.throughput.p95 << ','
             << record.throughput.median_ci.lower << ','
             << record.throughput.median_ci.upper << ','
             << load_fractions[load] << ','
             << record.offered_loads[load] << ','
             << record.latency_percentiles[load][0] << ','
             << record.latency_percentiles[load][1] << ','
             << record.latency_percentiles[load][2] << ','
             << record.latency_percentiles[load][3] << ','
             << record.latency_percentiles[load][4] << ','
             << record.latency_maximum[load] << '\n';
    }
  }
  if (!output) {
    std::cerr << "Failed while writing CSV output: " << path << '\n';
    std::exit(2);
  }
}

[[nodiscard]] Record benchmark_queue(const Workload& workload,
                                     const Options& options, bool ring_buffer,
                                     std::uint64_t seed) {
  Record record{workload.scenario,
                ring_buffer ? "ring_buffer" : "mutex_deque",
                workload.producers,
                workload.consumers,
                options.capacity,
                0.0,
                0.0,
                {},
                {},
                {},
                {},
                {}};
  std::vector<double> rates;
  rates.reserve(options.repetitions);

  for (std::size_t repetition = 0; repetition < options.repetitions;
       ++repetition) {
    if (options.warmup_seconds > 0.0) {
      RunConfig warmup{workload.producers, workload.consumers, 0,
                       options.warmup_seconds, 0.0, options.pin};
      RunResult warmup_result = ring_buffer
                                    ? dispatch_capacity(options.capacity,
                                          [&]<std::size_t Capacity>() {
                                            return run_ring<Capacity>(warmup);
                                          })
                                    : run_mutex(warmup);
      if (!warmup_result.valid) {
        std::cerr << "INVALID warmup run: " << warmup_result.error << '\n';
        std::exit(3);
      }
    }

    RunConfig pilot_config{workload.producers, workload.consumers,
                           options.items, 0.0, 0.0, options.pin};
    RunResult pilot =
        ring_buffer
            ? dispatch_capacity(options.capacity,
                  [&]<std::size_t Capacity>() {
                    return run_ring<Capacity>(pilot_config);
                  })
            : run_mutex(pilot_config);
    if (!pilot.valid || pilot.seconds <= 0.0) {
      std::cerr << "INVALID calibration run: " << pilot.error << '\n';
      std::exit(3);
    }
    const double rounds_as_double =
        std::ceil(options.duration_seconds / pilot.seconds);
    const std::size_t max_rounds =
        std::numeric_limits<std::size_t>::max() /
        (options.items * workload.producers);
    if (!std::isfinite(rounds_as_double) || rounds_as_double < 1.0 ||
        rounds_as_double > static_cast<double>(max_rounds)) {
      std::cerr << "Requested duration would exceed supported workload size\n";
      std::exit(2);
    }
    const auto rounds = static_cast<std::size_t>(rounds_as_double);
    const std::size_t measured_items_each = options.items * rounds;
    RunConfig measured{workload.producers, workload.consumers,
                       measured_items_each, 0.0, 0.0, options.pin};
    RunResult run =
        ring_buffer
            ? dispatch_capacity(options.capacity,
                  [&]<std::size_t Capacity>() {
                    return run_ring<Capacity>(measured);
                  })
            : run_mutex(measured);
    if (!run.valid) {
      std::cerr << "INVALID measured run (" << record.queue << ", "
                << workload.scenario << "): " << run.error << '\n';
      std::exit(3);
    }
    const double rate = static_cast<double>(run.delivered) / run.seconds;
    rates.push_back(rate);
    record.repetitions.push_back(rate);
    std::cout << std::left << std::setw(12) << workload.scenario << std::setw(14)
              << record.queue << " repetition " << repetition + 1 << ": "
              << std::fixed << std::setprecision(0) << rate << " items/s\n";
  }

  record.throughput = summarize(rates, seed);
  record.median_items_per_second = record.throughput.median;
  if (!run_latency(record, workload, options,
                   record.median_items_per_second, ring_buffer)) {
    std::exit(3);
  }
  return record;
}

int main_impl(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage(std::cerr);
    return 2;
  }
  warn_if_invalid_build(options);

  std::cout << "PeakPulse performance harness (schema 1)\n"
            << "Compiler: " << compiler_description() << "\n"
            << "Build: " << PEAK_PULSE_BUILD_TYPE << " | CPU: " << cpu_model()
            << " | cores: " << std::thread::hardware_concurrency() << "\n"
            << "OS: " << operating_system() << " | git: "
            << PEAK_PULSE_GIT_COMMIT << " | seed: " << options.seed << "\n"
            << "Scenario     Queue         Throughput (items/s, median)\n";

  std::vector<Record> records;
  for (const Workload& workload : workloads_for(options)) {
    if (workload.producers > 256 || workload.consumers > 256) {
      std::cerr << "Scenario exceeds 256 producer/consumer threads\n";
      return 2;
    }
    Record ring =
        benchmark_queue(workload, options, true, options.seed + records.size());
    Record baseline = benchmark_queue(
        workload, options, false, options.seed + records.size() + 1);
    const double speedup =
        baseline.median_items_per_second > 0.0
            ? ring.median_items_per_second /
                  baseline.median_items_per_second
            : 0.0;
    ring.speedup_vs_mutex = speedup;
    baseline.speedup_vs_mutex = 1.0;
    std::cout << std::left << std::setw(12) << workload.scenario
              << std::setw(14) << "ring_buffer" << std::fixed
              << std::setprecision(0) << ring.median_items_per_second
              << " | mutex_deque " << baseline.median_items_per_second
              << " | speedup " << std::setprecision(3) << speedup << "x\n";
    records.push_back(std::move(ring));
    records.push_back(std::move(baseline));
  }
  if (!options.json_path.empty()) {
    write_json(options.json_path, records, options);
  }
  if (!options.csv_path.empty()) {
    write_csv(options.csv_path, records);
  }
  return 0;
}

}

int main(int argc, char** argv) {
  return main_impl(argc, argv);
}
