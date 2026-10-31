#include "peak_pulse/perf_statistics.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace peak_pulse::perf {
namespace {

TEST(PerfStatisticsTests, ComputesMedianForOddAndEvenInputs) {
  constexpr std::array<double, 5> odd{9.0, 1.0, 5.0, 3.0, 7.0};
  constexpr std::array<double, 4> even{8.0, 2.0, 6.0, 4.0};
  EXPECT_EQ(median(odd), 5.0);
  EXPECT_EQ(median(even), 5.0);
  EXPECT_FALSE(median(std::span<const double>{}).has_value());
}

TEST(PerfStatisticsTests, ComputesNearestRankPercentile) {
  constexpr std::array<double, 5> values{10.0, 20.0, 30.0, 40.0, 50.0};
  EXPECT_EQ(percentile(values, 0.0), 10.0);
  EXPECT_EQ(percentile(values, 0.5), 30.0);
  EXPECT_EQ(percentile(values, 1.0), 50.0);
  EXPECT_FALSE(percentile(values, 1.1).has_value());
}

TEST(PerfStatisticsTests, BootstrapIntervalIsReproducibleAndOrdered) {
  constexpr std::array<double, 5> values{1.0, 2.0, 3.0, 4.0, 5.0};
  const auto first = bootstrap_median_ci(values, 500, 12345);
  const auto second = bootstrap_median_ci(values, 500, 12345);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_DOUBLE_EQ(first->lower, second->lower);
  EXPECT_DOUBLE_EQ(first->upper, second->upper);
  EXPECT_LE(first->lower, 3.0);
  EXPECT_GE(first->upper, 3.0);
  EXPECT_FALSE(bootstrap_median_ci(values, 0, 1).has_value());
}

TEST(PerfStatisticsTests, BucketsAndReportsLogarithmicPercentiles) {
  EXPECT_EQ(LogHistogram::bucket_for(0), 0U);
  EXPECT_EQ(LogHistogram::bucket_for(1), 1U);
  EXPECT_EQ(LogHistogram::bucket_for(3), 193U);

  LogHistogram histogram;
  histogram.record(0);
  histogram.record(1);
  histogram.record(3);
  histogram.record(7);
  EXPECT_EQ(histogram.count(), 4U);
  EXPECT_EQ(histogram.percentile_value(0.0), 0U);
  EXPECT_EQ(histogram.percentile_value(0.5), 1U);
  EXPECT_EQ(histogram.percentile_value(1.0), 7U);
  EXPECT_EQ(histogram.maximum(), 7U);

  LogHistogram positive_only;
  positive_only.record(5);
  EXPECT_EQ(positive_only.percentile_value(0.0), 5U);
}

}
}
