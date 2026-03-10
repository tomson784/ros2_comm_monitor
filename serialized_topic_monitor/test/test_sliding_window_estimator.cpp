#include <gtest/gtest.h>

#include <rclcpp/time.hpp>

#include "serialized_topic_monitor/serialized_topic_monitor.hpp"

using serialized_topic_monitor::SlidingWindowEstimator;

TEST(SlidingWindowEstimatorTest, ReturnsZeroWhenInsufficientSamples)
{
  // Intent: rate calculations are undefined with < 2 samples,
  // so estimator must return zeros and still track last size.
  SlidingWindowEstimator est(20U);
  EXPECT_DOUBLE_EQ(est.hz(), 0.0);
  EXPECT_DOUBLE_EQ(est.bandwidth_bytes_per_sec(), 0.0);
  EXPECT_DOUBLE_EQ(est.latest_message_size_bytes(), 0.0);

  est.tick(rclcpp::Time(1'000'000'000LL), 42U);
  EXPECT_DOUBLE_EQ(est.hz(), 0.0);
  EXPECT_DOUBLE_EQ(est.bandwidth_bytes_per_sec(), 0.0);
  EXPECT_DOUBLE_EQ(est.latest_message_size_bytes(), 42.0);
}

TEST(SlidingWindowEstimatorTest, ComputesHzAndBandwidth)
{
  // Intent: verify core formulas with deterministic timestamps/sizes.
  // Here samples are 1Hz and bytes/s is (200 + 300) / 2s = 250.
  SlidingWindowEstimator est(20U);
  est.tick(rclcpp::Time(0LL), 100U);
  est.tick(rclcpp::Time(1'000'000'000LL), 200U);
  est.tick(rclcpp::Time(2'000'000'000LL), 300U);

  EXPECT_NEAR(est.hz(), 1.0, 1e-9);
  EXPECT_NEAR(est.bandwidth_bytes_per_sec(), 250.0, 1e-9);
  EXPECT_DOUBLE_EQ(est.latest_message_size_bytes(), 300.0);
}

TEST(SlidingWindowEstimatorTest, TrimsWindowAndKeepsLatestSamples)
{
  // Intent: window trimming should drop oldest samples while preserving
  // latest metrics and allowing runtime window resize.
  SlidingWindowEstimator est(3U);
  est.tick(rclcpp::Time(0LL), 10U);
  est.tick(rclcpp::Time(1'000'000'000LL), 20U);
  est.tick(rclcpp::Time(2'000'000'000LL), 30U);
  est.tick(rclcpp::Time(3'000'000'000LL), 40U);

  EXPECT_NEAR(est.hz(), 1.0, 1e-9);
  EXPECT_NEAR(est.bandwidth_bytes_per_sec(), 35.0, 1e-9);
  EXPECT_DOUBLE_EQ(est.latest_message_size_bytes(), 40.0);

  est.set_window_size(2U);
  EXPECT_NEAR(est.hz(), 1.0, 1e-9);
  EXPECT_NEAR(est.bandwidth_bytes_per_sec(), 40.0, 1e-9);
  EXPECT_DOUBLE_EQ(est.latest_message_size_bytes(), 40.0);
}
