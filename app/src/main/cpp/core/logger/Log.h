#pragma once
#include <android/log.h>

#define XTERM_TAG "xterm-native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  XTERM_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  XTERM_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, XTERM_TAG, __VA_ARGS__)
