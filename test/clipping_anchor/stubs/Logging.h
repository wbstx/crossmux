#pragma once

template <typename... Args>
inline void clippingTestLog(const Args&...) {}

#define LOG_ERR(...) clippingTestLog(__VA_ARGS__)
#define LOG_INF(...) clippingTestLog(__VA_ARGS__)
#define LOG_DBG(...) clippingTestLog(__VA_ARGS__)
