// <time.h>, <sys/time.h>, mach time, sleeping.
#include "hle.h"
#include "modules.h"
#include <atomic>
#include <cerrno>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <thread>
#include <windows.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace libc {

namespace {

// Darwin arm64 struct tm (56 bytes).
struct DarwinTm {
    s32 tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    s32 pad;
    s64 tm_gmtoff;
    const char* tm_zone;
};
static_assert(sizeof(DarwinTm) == 56);

void to_darwin(DarwinTm* out, const std::tm& t, s64 gmtoff, const char* zone) {
    out->tm_sec = t.tm_sec;
    out->tm_min = t.tm_min;
    out->tm_hour = t.tm_hour;
    out->tm_mday = t.tm_mday;
    out->tm_mon = t.tm_mon;
    out->tm_year = t.tm_year;
    out->tm_wday = t.tm_wday;
    out->tm_yday = t.tm_yday;
    out->tm_isdst = t.tm_isdst;
    out->pad = 0;
    out->tm_gmtoff = gmtoff;
    out->tm_zone = zone;
}

#if IB3_NATIVE_CPU
// The guest reads the CPU's own counter (cntvct_el0) directly; its rate varies by device
// (19.2 MHz on Snapdragon) where Apple's is 24 MHz, so mach_timebase_info reports the real one.
u64 g_tick_hz = 24000000;
u64 ticks() {
    u64 v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

void precise_sleep_us(u64 us) {
    u64 end = ticks() + us * g_tick_hz / 1000000;
    if (us > 300) {
        timespec ts{(time_t)((us - 200) / 1000000), (long)((us - 200) % 1000000) * 1000};
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        }
    }
    while (ticks() < end) std::this_thread::yield();
}

void keep_timer_resolution() {}

// The counter's real rate, timed against the kernel's clock. cntfrq_el0 is set by the phone's firmware,
// and Samsung's Exynos firmware leaves it wrong (Linux takes the rate from the device tree there): the game
// then thought a frame took far less time than it did and spun ~1 s on every frame (S21, Exynos 2100).
u64 measure_tick_hz() {
    auto ns = [] { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec; };
    // A read pair bracketed as tightly as possible: the counter read sits between two clock reads.
    auto sample = [&](u64& t_ns, u64& tick) {
        u64 best = ~0ull;
        for (int i = 0; i < 5; i++) {
            u64 a = ns(), c = ticks(), b = ns();
            if (b - a < best) best = b - a, t_ns = a + (b - a) / 2, tick = c;
        }
    };
    u64 t0, c0, t1, c1;
    sample(t0, c0);
    timespec wait{0, 20 * 1000000};
    while (nanosleep(&wait, &wait) == -1 && errno == EINTR) {
    }
    sample(t1, c1);
    if (t1 <= t0 || c1 <= c0) return 0;
    double hz = (double)(c1 - c0) * 1e9 / (double)(t1 - t0);
    for (double known : {19200000.0, 24000000.0, 24576000.0, 25000000.0, 26000000.0, 38400000.0, 52000000.0, 1000000000.0})
        if (std::abs(hz - known) < known * 0.005) return (u64)known;
    return (u64)(hz / 1000 + 0.5) * 1000;
}

void calibrate_tick_hz() {
    asm volatile("mrs %0, cntfrq_el0" : "=r"(g_tick_hz));
    u64 reported = g_tick_hz;
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";  // debug.ibport.tickhz N: pretend the firmware reports N (to test this)
    if (__system_property_get("debug.ibport.tickhz", v) > 0 && atoll(v) > 0) reported = (u64)atoll(v);
#endif
    u64 measured = measure_tick_hz();
    bool wrong = measured && (reported == 0 || std::abs((double)reported - (double)measured) > measured * 0.02);
#ifdef __ANDROID__
    if (__system_property_get("debug.ibport.tickfix", v) > 0 && v[0] == '0') wrong = false;  // old behaviour
#endif
    g_tick_hz = wrong ? measured : reported ? reported : 24000000;
    if (wrong) LOG_WARN("timer: the CPU reports a %.3f MHz counter but it runs at %.3f MHz; using the measured rate",
                        reported / 1e6, measured / 1e6);
    else LOG_INFO("timer: %.3f MHz counter (measured %.3f MHz)", g_tick_hz / 1e6, measured / 1e6);
}
#else
LARGE_INTEGER g_freq;
constexpr u64 g_tick_hz = 24000000;
u64 ticks() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (u64)((unsigned __int128)now.QuadPart * 24000000 / g_freq.QuadPart);
}

// Precise sleep: a high-resolution waitable timer (accurate whatever the system timer resolution)
// for all but the last half millisecond, then spin the remainder.
void precise_sleep_us(u64 us) {
    u64 end = ticks() + us * 24;
    if (us > 1000) {
        constexpr DWORD kHighResolution = 0x2;  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+)
        thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, kHighResolution, TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((us - 500) * 10);  // relative, in 100 ns units
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep((DWORD)(us / 1000 - 1));
    }
    while (ticks() < end) std::this_thread::yield();
}

// Windows 11 stops honouring timeBeginPeriod for processes whose window is hidden, minimized or
// occluded, which stretches the engine's 1 ms waits (and the frame limiter's sleeps) to ~15.6 ms.
void keep_timer_resolution() {
    struct ThrottlingState {
        ULONG version, control_mask, state_mask;
    } state{1 /* PROCESS_POWER_THROTTLING_CURRENT_VERSION */, 0x4 /* ..._IGNORE_TIMER_RESOLUTION */, 0};
    using SetInfo = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    auto set = reinterpret_cast<SetInfo>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"));
    if (set) set(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &state, sizeof state);
}
#endif

std::tm host_localtime(s64 t) {
    std::tm tm{};
#ifdef _WIN32
    __time64_t tt = t;
    _localtime64_s(&tm, &tt);
#else
    time_t tt = (time_t)t;
    localtime_r(&tt, &tm);
#endif
    return tm;
}

std::tm host_gmtime(s64 t) {
    std::tm tm{};
#ifdef _WIN32
    __time64_t tt = t;
    _gmtime64_s(&tm, &tt);
#else
    time_t tt = (time_t)t;
    gmtime_r(&tt, &tm);
#endif
    return tm;
}

s64 local_gmtoff(const std::tm& tm) {
#ifdef _WIN32
    long tz;
    _get_timezone(&tz);
    return -(s64)tz + (tm.tm_isdst > 0 ? 3600 : 0);
#else
    return tm.tm_gmtoff;
#endif
}

}  // namespace

u64 mach_ticks() { return ticks(); }

void install_time() {
    using hle::fn;
#if IB3_NATIVE_CPU
    calibrate_tick_hz();
#else
    QueryPerformanceFrequency(&g_freq);
#endif
    timeBeginPeriod(1);
    keep_timer_resolution();

    fn("_time", [](s64* out) -> s64 {
        s64 t = std::time(nullptr);
        if (out) *out = t;
        return t;
    });
    fn("_gettimeofday", [](void* tv, void*) {
        if (tv) {
            FILETIME ft;
            GetSystemTimePreciseAsFileTime(&ft);
            u64 t = ((u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10 - 11644473600000000ull;  // us since 1970
            *static_cast<s64*>(tv) = (s64)(t / 1000000);
            *reinterpret_cast<s32*>(static_cast<u8*>(tv) + 8) = (s32)(t % 1000000);
        }
        return 0;
    });
    fn("_localtime_r", [](const s64* t, DarwinTm* out) {
        std::tm tm = host_localtime(*t);
        to_darwin(out, tm, local_gmtoff(tm), hle::static_cstr("LOCAL"));
        return out;
    });
    fn("_gmtime_r", [](const s64* t, DarwinTm* out) {
        to_darwin(out, host_gmtime(*t), 0, hle::static_cstr("UTC"));
        return out;
    });
    fn("_gmtime", [](const s64* t) {
        static thread_local DarwinTm* buf = static_cast<DarwinTm*>(std::calloc(1, sizeof(DarwinTm)));
        to_darwin(buf, host_gmtime(*t), 0, hle::static_cstr("UTC"));
        return buf;
    });
    fn("_mktime", [](DarwinTm* in) -> s64 {
        std::tm tm{};
        tm.tm_sec = in->tm_sec;
        tm.tm_min = in->tm_min;
        tm.tm_hour = in->tm_hour;
        tm.tm_mday = in->tm_mday;
        tm.tm_mon = in->tm_mon;
        tm.tm_year = in->tm_year;
        tm.tm_isdst = in->tm_isdst;
#ifdef _WIN32
        return _mktime64(&tm);
#else
        return (s64)mktime(&tm);
#endif
    });
    fn("_difftime", [](s64 a, s64 b) { return (double)(a - b); });

    // Called millions of times per second; read the counter without leaving the JIT.
#if IB3_NATIVE_CPU
    hle::native("_mach_absolute_time", {
        0xd53be040,  // mrs x0, cntvct_el0
        0xd65f03c0,  // ret
    });
#else
    hle::native("_mach_absolute_time", {
        0xd53be020,  // mrs x0, cntpct_el0
        0xd65f03c0,  // ret
    });
#endif
    fn("_mach_timebase_info", [](u32* info) {
        u64 numer = 1000000000, denom = g_tick_hz;  // ticks -> ns (125/3 at 24 MHz)
        u64 g = std::gcd(numer, denom);
        info[0] = (u32)(numer / g);
        info[1] = (u32)(denom / g);
        return 0;
    });
    fn("_mach_wait_until", [](cpu::Thread& t, u64 deadline) {
        u64 now = ticks();
        static std::atomic<u64> calls{0};
        if (logging::enabled(logging::Level::Debug) && calls++ % 300 == 0)
            LOG_DEBUG("mach_wait_until: %.3f ms\n%s", deadline > now ? (deadline - now) * 1000.0 / g_tick_hz : 0.0,
                      t.backtrace().c_str());
        if (deadline > now) precise_sleep_us((deadline - now) * 1000000 / g_tick_hz);
        return 0;
    });
    fn("_usleep", [](u32 us) {
        precise_sleep_us(us);
        return 0;
    });
    fn("_sched_yield", []() {
        static std::atomic<u64> calls{0};
        if (logging::enabled(logging::Level::Debug) && calls++ % 200000 == 0)
            LOG_DEBUG("sched_yield from %s", cpu::symbolize(cpu::current().x(30)).c_str());
        SwitchToThread();
        return 0;
    });
    fn("_pause", []() {
        Sleep(INFINITE);
        return 0;
    });
    fn("_CFAbsoluteTimeGetCurrent", []() {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        u64 t = ((u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime);  // 100ns since 1601
        return (double)(t - 126227808000000000ull) / 1e7;           // seconds since 2001-01-01
    });
}

}  // namespace libc
