add_executable(ClippingAnchorTest
  ClippingAnchorTest.cpp
)

target_include_directories(ClippingAnchorTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ClippingAnchorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClippingAnchorTest)
