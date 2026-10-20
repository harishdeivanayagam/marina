// marina runtime — std modules: time (wall clock, monotonic clock, sleep,
// ISO-8601 formatting) and os (platform info, environment variables,
// process ops). Pure POSIX; no external libraries.
#include "runtime_internal.hpp"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include <signal.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace marina_rt {

namespace {

// Wrap a signed int64 for a TAG_INT MValue (pad 64 = Int64, signed).
MValue mv_i64(int64_t v) { return MValue{TAG_INT, 64, v, 0.0, nullptr}; }

int64_t epoch_nanos() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               now.time_since_epoch())
        .count();
}

int64_t mono_millis() {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch())
        .count();
}

} // namespace

} // namespace marina_rt

// ---------------------------------------------------------------------------
// C ABI: std module: time
// ---------------------------------------------------------------------------

// `time.now()` — Unix epoch milliseconds (wall clock).
extern "C" void marina_time_now(MValue* out) {
    *out = marina_rt::mv_i64(marina_rt::epoch_nanos() / 1000000);
}

// `time.nowSecs()` — Unix epoch seconds (wall clock).
extern "C" void marina_time_now_secs(MValue* out) {
    *out = marina_rt::mv_i64(marina_rt::epoch_nanos() / 1000000000);
}

// `time.nowNanos()` — Unix epoch nanoseconds (wall clock).
extern "C" void marina_time_now_nanos(MValue* out) {
    *out = marina_rt::mv_i64(marina_rt::epoch_nanos());
}

// `time.mono()` — monotonic milliseconds; unaffected by system clock
// changes, meant for measuring elapsed time.
extern "C" void marina_time_mono(MValue* out) {
    *out = marina_rt::mv_i64(marina_rt::mono_millis());
}

// `time.sleep(ms)` — block the calling thread for `ms` milliseconds
// (negative ms treated as 0).
extern "C" void marina_time_sleep(int64_t ms) {
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

// `time.format(ms)` — ISO-8601 UTC for epoch milliseconds, e.g.
// "2026-10-20T15:04:05.123Z". Negative values format the pre-epoch date.
extern "C" void marina_time_format(int64_t ms, MValue* out) {
    int64_t secs = ms / 1000;
    int64_t millis = ms % 1000;
    if (millis < 0) {
        millis += 1000;
        secs -= 1;
    }
    std::time_t t = (std::time_t)secs;
    std::tm tmv{};
    if (::gmtime_r(&t, &tmv) == nullptr) {
        marina_rt::rt_panic("time.format: cannot format " + std::to_string(ms));
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                  tmv.tm_min, tmv.tm_sec, (int)millis);
    marina_rt::out_mv(out, marina_rt::mv_str_owned(std::string(buf)));
}

// ---------------------------------------------------------------------------
// C ABI: std module: os
// ---------------------------------------------------------------------------

// `os.platform()` — "macos", "linux" or "windows" (raw uname sysname
// lowercased when unknown).
extern "C" void marina_os_platform(MValue* out) {
    struct utsname u{};
    if (::uname(&u) != 0) {
        marina_rt::rt_panic("os.platform: uname failed (" +
                            marina_rt::os_error(errno) + ")");
    }
    std::string sys = u.sysname;
    for (char& c : sys) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    std::string platform;
    if (sys == "darwin") platform = "macos";
    else if (sys == "linux") platform = "linux";
    else if (sys == "windows" || sys == "msys" || sys == "mingw" ||
             sys == "cygwin")
        platform = "windows";
    else platform = sys;
    marina_rt::out_mv(out, marina_rt::mv_str_owned(platform));
}

// `os.arch()` — machine architecture from uname ("arm64", "x86_64", ...).
extern "C" void marina_os_arch(MValue* out) {
    struct utsname u{};
    if (::uname(&u) != 0) {
        marina_rt::rt_panic("os.arch: uname failed (" +
                            marina_rt::os_error(errno) + ")");
    }
    marina_rt::out_mv(out, marina_rt::mv_str_owned(std::string(u.machine)));
}

// `os.hostname()` — the machine's host name ("" when unavailable).
extern "C" void marina_os_hostname(MValue* out) {
    char buf[256];
    if (::gethostname(buf, sizeof buf) != 0) {
        buf[0] = '\0';
    }
    buf[sizeof buf - 1] = '\0';
    marina_rt::out_mv(out, marina_rt::mv_str_owned(std::string(buf)));
}

// `os.cwd()` — current working directory.
extern "C" void marina_os_cwd(MValue* out) {
    char buf[4096];
    if (::getcwd(buf, sizeof buf) == nullptr) {
        marina_rt::rt_panic("os.cwd: cannot get working directory (" +
                            marina_rt::os_error(errno) + ")");
    }
    marina_rt::out_mv(out, marina_rt::mv_str_owned(std::string(buf)));
}

// `os.tmpdir()` — $TMPDIR, falling back to "/tmp".
extern "C" void marina_os_tmpdir(MValue* out) {
    const char* tmp = ::getenv("TMPDIR");
    marina_rt::out_mv(out, marina_rt::mv_str_owned(
                               std::string((tmp != nullptr && tmp[0] != '\0') ? tmp : "/tmp")));
}

// `os.pid()` — the process id.
extern "C" void marina_os_pid(MValue* out) {
    *out = marina_rt::mv_i64((int64_t)::getpid());
}

// `os.env(name)` — an environment variable; panics when unset (use
// os.envOr for a fallback).
extern "C" void marina_os_env(const MValue* name, MValue* out) {
    std::string n = marina_rt::mv_to_string(*name);
    const char* v = ::getenv(n.c_str());
    if (v == nullptr) {
        marina_rt::rt_panic("os.env '" + n + "': not set");
    }
    marina_rt::out_mv(out, marina_rt::mv_str_owned(std::string(v)));
}

// `os.envOr(name, fallback)` — like os.env but returns `fallback` when
// unset.
extern "C" void marina_os_env_or(const MValue* name, const MValue* fallback,
                                 MValue* out) {
    std::string n = marina_rt::mv_to_string(*name);
    const char* v = ::getenv(n.c_str());
    if (v != nullptr) {
        marina_rt::out_mv(out, marina_rt::mv_str_owned(std::string(v)));
    } else {
        marina_rt::out_mv(out,
                          marina_rt::mv_str_owned(marina_rt::mv_to_string(*fallback)));
    }
}

// `os.setEnv(name, value)` — set (or overwrite) an environment variable.
extern "C" void marina_os_set_env(const MValue* name, const MValue* value) {
    std::string n = marina_rt::mv_to_string(*name);
    std::string v = marina_rt::mv_to_string(*value);
    if (::setenv(n.c_str(), v.c_str(), 1) != 0) {
        marina_rt::rt_panic("os.setEnv '" + n + "': " + marina_rt::os_error(errno));
    }
}

// `os.unsetEnv(name)` — remove an environment variable.
extern "C" void marina_os_unset_env(const MValue* name) {
    std::string n = marina_rt::mv_to_string(*name);
    if (::unsetenv(n.c_str()) != 0) {
        marina_rt::rt_panic("os.unsetEnv '" + n + "': " + marina_rt::os_error(errno));
    }
}

// `os.exec(command)` — run `command` through /bin/sh and capture stdout.
// stderr passes through to the process's stderr; a non-zero exit status
// or a spawn failure panics.
extern "C" void marina_os_exec(const MValue* cmd, MValue* out) {
    std::string command = marina_rt::mv_to_string(*cmd);
    FILE* fp = ::popen(command.c_str(), "r");
    if (fp == nullptr) {
        marina_rt::rt_panic("os.exec '" + command + "': cannot spawn (" +
                            marina_rt::os_error(errno) + ")");
    }
    std::string result;
    char buf[4096];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof buf, fp)) > 0) result.append(buf, n);
    int status = ::pclose(fp);
    if (status != 0) {
        if (WIFEXITED(status)) {
            marina_rt::rt_panic("os.exec '" + command + "': exit status " +
                                std::to_string(WEXITSTATUS(status)));
        }
        marina_rt::rt_panic("os.exec '" + command + "': terminated by signal");
    }
    marina_rt::out_mv(out, marina_rt::mv_str_owned(result));
}

// `os.exit(code)` — terminate the process with the given exit status.
extern "C" void marina_os_exit(int64_t code) {
    std::fflush(nullptr);
    std::exit((int)code);
}
