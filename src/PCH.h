#pragma once

// ========================================
// Precompiled Header for FileTraceSKSE
// ========================================
// This PCH includes CommonLibSSE-NG headers.
// IMPORTANT: This file is used by CMake target_precompile_headers()
// DO NOT manually include this in your .cpp files - CMake handles it automatically

// Standard Library
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <algorithm>
#include <ranges>

// C++20 features
using namespace std::literals;

// CommonLibSSE-NG (automatically includes all RE:: and SKSE:: headers)
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// Version information
#define MAKE_STR_HELPER(x) #x
#define MAKE_STR(x) MAKE_STR_HELPER(x)

#define PLUGIN_NAME "FileTraceSKSE"
#define PLUGIN_AUTHOR "gkd2323c"
#define PLUGIN_VERSION MAKE_STR(1) "." MAKE_STR(0) "." MAKE_STR(0)
