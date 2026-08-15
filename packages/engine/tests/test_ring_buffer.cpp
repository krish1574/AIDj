#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "RingBuffer.h"

using aidj::RingBuffer;

TEST_CASE("RingBuffer rounds capacity up to a power of two") {
  RingBuffer buffer(1000);
  REQUIRE(buffer.capacity() == 1024);
}

TEST_CASE("RingBuffer round-trips data in order") {
  RingBuffer buffer(64);
  std::vector<float> source{1.0f, 2.0f, 3.0f, 4.0f};

  REQUIRE(buffer.write(source.data(), source.size()) == 4);
  REQUIRE(buffer.sizeAvailableToRead() == 4);

  std::vector<float> destination(4, 0.0f);
  REQUIRE(buffer.readOrSilence(destination.data(), 4) == 4);
  REQUIRE(destination == source);
  REQUIRE(buffer.sizeAvailableToRead() == 0);
}

TEST_CASE("RingBuffer refuses to overfill") {
  RingBuffer buffer(8);  // capacity 8, one slot reserved
  std::vector<float> source(16, 1.0f);

  const size_t written = buffer.write(source.data(), source.size());
  REQUIRE(written == 7);
}

TEST_CASE("RingBuffer zero-fills a short read rather than returning garbage") {
  RingBuffer buffer(64);
  std::vector<float> source{5.0f, 6.0f};
  buffer.write(source.data(), 2);

  std::vector<float> destination(6, 99.0f);
  const size_t read = buffer.readOrSilence(destination.data(), 6);

  REQUIRE(read == 2);
  REQUIRE(destination[0] == 5.0f);
  REQUIRE(destination[1] == 6.0f);
  // The shortfall must be silence. Anything else is an audible glitch.
  for (size_t i = 2; i < 6; ++i) {
    REQUIRE(destination[i] == 0.0f);
  }
}

TEST_CASE("RingBuffer wraps correctly across many cycles") {
  RingBuffer buffer(16);
  float expected = 0.0f;

  for (int cycle = 0; cycle < 100; ++cycle) {
    std::vector<float> source(8);
    for (size_t i = 0; i < source.size(); ++i) {
      source[i] = expected + static_cast<float>(i);
    }
    REQUIRE(buffer.write(source.data(), source.size()) == 8);

    std::vector<float> destination(8, 0.0f);
    REQUIRE(buffer.readOrSilence(destination.data(), 8) == 8);
    REQUIRE(destination == source);
    expected += 8.0f;
  }
}

TEST_CASE("RingBuffer survives concurrent producer and consumer") {
  RingBuffer buffer(1024);
  constexpr size_t kTotal = 200000;

  std::atomic<bool> mismatch{false};
  std::thread producer([&] {
    size_t sent = 0;
    while (sent < kTotal) {
      const float value = static_cast<float>(sent % 1000);
      if (buffer.write(&value, 1) == 1) ++sent;
      else std::this_thread::yield();
    }
  });

  size_t received = 0;
  while (received < kTotal) {
    float value = -1.0f;
    if (buffer.readOrSilence(&value, 1) == 1) {
      if (value != static_cast<float>(received % 1000)) mismatch.store(true);
      ++received;
    }
  }

  producer.join();
  REQUIRE_FALSE(mismatch.load());
}
