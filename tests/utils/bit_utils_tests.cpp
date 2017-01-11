//
// IResearch search engine 
// 
// Copyright (c) 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#include "tests_shared.hpp"

#include "utils/bit_utils.hpp"

#include <climits>

TEST(bit_utils_test, zig_zag_32) {
  using namespace iresearch;

  const int32_t min = std::numeric_limits< int32_t >::min();
  const int32_t max = std::numeric_limits< int32_t >::max();

  EXPECT_EQ(min, zig_zag_decode32(zig_zag_encode32(min)));
  EXPECT_EQ(max, zig_zag_decode32(zig_zag_encode32(max)));

  const int32_t step = 255;
  const int32_t range_min = -std::numeric_limits< int32_t >::min() / step;
  const int32_t range_max = std::numeric_limits< int32_t >::max() / step;

  for (int32_t i = range_min; i < range_max; i += step) {
    EXPECT_EQ(i, zig_zag_decode32(zig_zag_encode32(i)));
  }
}

TEST(bit_utils_test, zig_zag_64) {
  using namespace iresearch;

  const int64_t min = std::numeric_limits< int64_t >::min();
  const int64_t max = std::numeric_limits< int64_t >::max();

  EXPECT_EQ(min, zig_zag_decode64(zig_zag_encode64(min)));
  EXPECT_EQ(max, zig_zag_decode64(zig_zag_encode64(max)));

  const int64_t step = 255;
  const int64_t range_min = -std::numeric_limits< int32_t >::min() / step;
  const int64_t range_max = std::numeric_limits< int32_t >::max() / step;

  for (int64_t i = range_min; i < range_max; i += step) {
    EXPECT_EQ(i, zig_zag_decode64(zig_zag_encode64(i)));
  }
}

TEST(bit_utils_test, set_check_bit) {
  using namespace iresearch;

  int v = 3422;

  for (unsigned i = 0; i < sizeof(v) * 8; ++i) {
    EXPECT_EQ(static_cast<bool>((v >> i) & 1), check_bit(v, i));

    unset_bit(v, i);
    EXPECT_FALSE(check_bit(v, i));
  }
  EXPECT_EQ(0, v);

  byte_type b = 76;

  EXPECT_FALSE(check_bit<0>(b));
  EXPECT_FALSE(check_bit<1>(b));
  EXPECT_TRUE(check_bit<2>(b));
  EXPECT_TRUE(check_bit<3>(b));
  EXPECT_FALSE(check_bit<4>(b));
  EXPECT_FALSE(check_bit<5>(b));
  EXPECT_TRUE(check_bit<6>(b));
  EXPECT_FALSE(check_bit<7>(b));

  byte_type b_orig = b;
  set_bit<0>(!check_bit<0>(b), b);
  set_bit<0>(!check_bit<0>(b), b);
  set_bit<1>(!check_bit<1>(b), b);
  set_bit<1>(!check_bit<1>(b), b);
  set_bit<2>(!check_bit<2>(b), b);
  set_bit<2>(!check_bit<2>(b), b);
  set_bit<3>(!check_bit<3>(b), b);
  set_bit<3>(!check_bit<3>(b), b);
  set_bit<4>(!check_bit<4>(b), b);
  set_bit<4>(!check_bit<4>(b), b);
  set_bit<5>(!check_bit<5>(b), b);
  set_bit<5>(!check_bit<5>(b), b);
  set_bit<6>(!check_bit<6>(b), b);
  set_bit<6>(!check_bit<6>(b), b);
  set_bit<7>(!check_bit<7>(b), b);
  set_bit<7>(!check_bit<7>(b), b);
  EXPECT_EQ(b_orig, b);

}
