// The few operating system services the engine needs, for Windows and for macOS (and other POSIX).
// The emulation itself is plain C++; only waiting, timing and file renaming differ.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef min
#undef max
#pragma comment(lib, "synchronization.lib")
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define OB12_X86 1
#endif

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace ob12 {

// A spin-wait hint: PAUSE on x86, YIELD on ARM.
inline void cpu_relax() {
#if defined(OB12_X86)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

// A fast monotonic tick counter, only used for the timing study (OB12_TIME_LOG).
inline uint64_t ticks() {
#if defined(OB12_X86)
    return __rdtsc();
#elif defined(__APPLE__)
    return mach_absolute_time();
#else
    return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}

inline void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Wait while *address == seen, at most timeout_ms; wake_all ends the waits. On Windows the
// kernel's address wait. Elsewhere a short sleep: the engine's default layout has no worker
// threads, and with workers this only costs up to 0.2 ms of latency between bursts.
inline void wait_on(std::atomic<long>& address, long seen, int timeout_ms) {
#ifdef _WIN32
    WaitOnAddress(&address, &seen, sizeof seen, timeout_ms);
#else
    (void)timeout_ms;
    if (address.load(std::memory_order_acquire) == seen) std::this_thread::sleep_for(std::chrono::microseconds(200));
#endif
}
inline void wake_all(std::atomic<long>& address) {
#ifdef _WIN32
    WakeByAddressAll(&address);
#else
    (void)address;
#endif
}

// Replace a file by another, atomically where the system allows.
inline bool replace_file(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}
inline void delete_file(const std::string& path) { std::remove(path.c_str()); }

}  // namespace ob12
