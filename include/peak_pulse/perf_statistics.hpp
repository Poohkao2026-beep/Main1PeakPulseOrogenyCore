#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <vector>

namespace peak_pulse::perf {

struct ConfidenceInterval final {
  double lower;
  double upper;
};

[[nodiscard]] inline std::optional<double> median(std::span<const double> input) {
  if (input.empty()) {
    return std::nullopt;
  }
  std::vector<double> sorted(input.begin(), input.end());
  std::sort(sorted.begin(), sorted.end());
  const std::size_t middle = sorted.size() / 2;
  if ((sorted.size() & 1U) != 0U) {
    return sorted[middle];
  }
  return (sorted[middle - 1] + sorted[middle]) / 2.0;
}

[[nodiscard]] inline std::optional<double> percentile(
    std::span<const double> input, double fraction) {
  if (input.empty() || !std::isfinite(fraction) || fraction < 0.0 ||
      fraction > 1.0) {
    return std::nullopt;
  }
  std::vector<double> sorted(input.begin(), input.end());
  std::sort(sorted.begin(), sorted.end());
  const double index = fraction * static_cast<double>(sorted.size() - 1);
  return sorted[static_cast<std::size_t>(std::ceil(index))];
}

[[nodiscard]] inline std::optional<ConfidenceInterval> bootstrap_median_ci(
    std::span<const double> input, std::size_t resamples, std::uint64_t seed) {
  if (input.empty() || resamples == 0) {
    return std::nullopt;
  }
  std::mt19937_64 generator(seed);
  std::uniform_int_distribution<std::size_t> select(0, input.size() - 1);
  std::vector<double> sample(input.size());
  std::vector<double> medians;
  medians.reserve(resamples);
  for (std::size_t iteration = 0; iteration < resamples; ++iteration) {
    for (double& value : sample) {
      value = input[select(generator)];
    }
    medians.push_back(*median(sample));
  }
  const auto lower = percentile(medians, 0.025);
  const auto upper = percentile(medians, 0.975);
  return ConfidenceInterval{*lower, *upper};
}

class LogHistogram final {
 public:
  static constexpr std::size_t sub_buckets = 128;
  static constexpr std::size_t bucket_count = 1 + 64 * sub_buckets;

  [[nodiscard]] static constexpr std::size_t bucket_for(
      std::uint64_t value) noexcept {
    if (value == 0) {
      return 0;
    }
    const unsigned exponent = std::bit_width(value) - 1U;
    const std::uint64_t base = std::uint64_t{1} << exponent;
    const std::uint64_t delta = value - base;
    const std::uint64_t sub_bucket =
        exponent >= 7U ? delta >> (exponent - 7U)
                       : delta << (7U - exponent);
    return 1U + static_cast<std::size_t>(exponent) * sub_buckets +
           static_cast<std::size_t>(sub_bucket);
  }

  void record(std::uint64_t value) noexcept {
    ++buckets_[bucket_for(value)];
    ++count_;
    if (value > maximum_) {
      maximum_ = value;
    }
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t maximum() const noexcept { return maximum_; }

  [[nodiscard]] std::uint64_t percentile_value(double fraction) const noexcept {
    if (count_ == 0 || !std::isfinite(fraction) || fraction < 0.0 ||
        fraction > 1.0) {
      return 0;
    }
    const auto rank = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(
               std::ceil(fraction * static_cast<double>(count_))));
    std::uint64_t cumulative = 0;
    for (std::size_t bucket = 0; bucket < bucket_count; ++bucket) {
      cumulative += buckets_[bucket];
      if (cumulative >= rank) {
        if (bucket == 0) {
          return 0;
        }
        if (bucket == bucket_count - 1) {
          return UINT64_MAX;
        }
        const std::size_t logarithmic_bucket = bucket - 1;
        const unsigned exponent = static_cast<unsigned>(
            logarithmic_bucket / sub_buckets);
        const std::uint64_t sub_bucket =
            static_cast<std::uint64_t>(logarithmic_bucket % sub_buckets);
        const std::uint64_t base = std::uint64_t{1} << exponent;
        if (exponent < 7U) {
          return base + (sub_bucket >> (7U - exponent));
        }
        const std::uint64_t width = std::uint64_t{1} << (exponent - 7U);
        return base + (sub_bucket + 1U) * width - 1U;
      }
    }
    return maximum_;
  }

 private:
  std::array<std::uint64_t, bucket_count> buckets_{};
  std::uint64_t count_{0};
  std::uint64_t maximum_{0};
};

}
