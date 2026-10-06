# Compile an unchanged copy so ClippingsManager's quoted ../CrossPointSettings.h resolves to a test double.
configure_file(${REPO_ROOT}/src/clippings/ClippingsManager.cpp clippings/ClippingsManager.cpp COPYONLY)
configure_file(${REPO_ROOT}/src/clippings/ClippingsManager.h clippings/ClippingsManager.h COPYONLY)
configure_file(${REPO_ROOT}/src/ClippingStore.h ClippingStore.h COPYONLY)
configure_file(stubs/CrossPointSettings.h CrossPointSettings.h COPYONLY)

add_executable(ClippingAnchorTest
  ClippingAnchorTest.cpp
  ClippingExportTest.cpp
  ${REPO_ROOT}/src/clippings/ClipTextBuilder.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/clippings/ClippingsManager.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ClippingAnchorTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_include_directories(ClippingAnchorTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(ClippingAnchorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClippingAnchorTest)
