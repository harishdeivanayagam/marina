// marina runtime — internal shared declarations.
// Not part of the stable ABI; only included by runtime/src/*.cpp.
#ifndef MARINA_RUNTIME_INTERNAL_HPP
#define MARINA_RUNTIME_INTERNAL_HPP

#include "marina_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace marina_rt {

// ---------------------------------------------------------------------------
// ARC object layouts (must match the Rust crate's Header-based scheme)
// ---------------------------------------------------------------------------

constexpr uint8_t KIND_STRING = 0;
constexpr uint8_t KIND_PROMISE = 1;
constexpr uint8_t KIND_ARRAY = 2;
constexpr uint8_t KIND_OBJECT = 3;

// Every ref-counted heap object starts with this header.
// `rc < 0` means immortal (interned string literals — `release` on them
// is a no-op). std::atomic<int64_t> is lock-free on our 64-bit targets.
struct Header {
    std::atomic<int64_t> rc;
    uint8_t kind;
    uint8_t pad[7];
};
static_assert(sizeof(Header) == 16, "Header layout");

struct StrObj {
    Header h;
    uint64_t len;
    // UTF-8 bytes follow the struct
};
constexpr size_t STR_HDR = sizeof(StrObj);

using ResumeFn = void (*)(void* frame, int32_t state, const MValue* value);

struct Waiter {
    ResumeFn resume;
    void* frame;
    int32_t state;
    // Set by `marina_await`: this waiter took over the awaiting frame's
    // reference to the promise (the await consumes the caller's ref), so
    // the scheduled waiter task owns it and releases it after resume.
    bool owns_promise_ref;
};

struct PromiseInner {
    std::mutex mu;
    bool done = false;
    MValue value{};
    std::vector<Waiter> waiters;
};

struct Promise {
    Header h;
    PromiseInner inner;
};

struct ArrayObj {
    Header h;
    int32_t tag;
    int32_t _pad;
    std::vector<MValue> data;
};

struct ObjectObj {
    Header h;
    int32_t class_id;
    int32_t _pad;
    std::vector<MValue> data;
};

// ---------------------------------------------------------------------------
// Class registry (ghost class metadata)
// ---------------------------------------------------------------------------

struct ClassInfo {
    std::string name;
    std::vector<std::string> fields;
};
std::mutex& class_mutex();
std::unordered_map<int32_t, ClassInfo>& class_map();

// ---------------------------------------------------------------------------
// Core helpers (core.cpp)
// ---------------------------------------------------------------------------

[[noreturn]] void rt_panic(const std::string& msg);

// Render an errno the way Rust's std::io::Error displays OS errors:
// "<strerror> (os error <n>)".
std::string os_error(int err);

// Rust's saturating `f64 as i64` cast (NaN -> 0, +/-inf -> i64::MIN/MAX).
int64_t sat_i64(double f);

// Rust's String::from_utf8_lossy: invalid sequences become U+FFFD.
std::string utf8_lossy(const uint8_t* data, size_t len);

// The `{}` (Display) formatting of a marina value.
std::string value_to_string(const MValue& v);
// mv_to_string: strings decode lossily; other tags use value_to_string.
std::string mv_to_string(const MValue& v);
// Rust f64 Display formatting (never scientific notation).
std::string fmt_float(double f);
// Integer / float display that honors MValue.pad (width + unsigned bit).
inline std::string fmt_tagged_int(const MValue& v) {
    if ((v.pad & 0x100) != 0) return std::to_string(static_cast<uint64_t>(v.i));
    return std::to_string(static_cast<int64_t>(v.i));
}
inline std::string fmt_tagged_float(const MValue& v) {
    if (v.pad == 32) return fmt_float(static_cast<double>(static_cast<float>(v.f)));
    return fmt_float(v.f);
}
// Strict Rust-style i64 parsing (used for HTTP status lines).
bool parse_i64_strict(const std::string& s, int64_t* out);
// Strict Rust-style usize parsing (accepts a leading '+').
bool parse_usize_strict(const std::string& s, size_t* out);

void* alloc_string(const uint8_t* bytes, size_t len, int64_t rc);
inline void* alloc_string(const std::string& s, int64_t rc) {
    return alloc_string(reinterpret_cast<const uint8_t*>(s.data()), s.size(), rc);
}
inline uint64_t str_len(void* obj) { return reinterpret_cast<StrObj*>(obj)->len; }
inline const uint8_t* str_bytes(void* obj) {
    return reinterpret_cast<const uint8_t*>(obj) + STR_HDR;
}

void retain_ptr(void* p);
void release_ptr(void* p);
void free_obj(void* p);

// Tags whose payload is an ARC-managed heap pointer (element rules).
inline bool is_ptr_tag(int32_t t) {
    return t == TAG_STRING || t == TAG_ARRAY || t == TAG_OBJECT;
}
// Tags the value-level helpers own (adds TAG_STREAM).
inline bool owns_ptr_tag(int32_t t) { return is_ptr_tag(t) || t == TAG_STREAM; }

void retain_value(const MValue& v);   // owns_ptr_tag set
void release_value(const MValue& v);  // owns_ptr_tag set
void retain_payload(const MValue& v); // is_ptr_tag set, immortal-safe
void release_elements(const std::vector<MValue>& vs);
inline void drop_mv(const MValue& v) {
    if (is_ptr_tag(v.tag) && v.p != nullptr) release_ptr(v.p);
}

inline MValue mv_void() { return MValue{TAG_VOID, 0, 0, 0.0, nullptr}; }
inline MValue mv_int(int64_t i) { return MValue{TAG_INT, 0, i, 0.0, nullptr}; }
inline MValue mv_bool(bool b) { return MValue{TAG_BOOL, 0, b ? 1 : 0, 0.0, nullptr}; }
MValue mv_str_owned(const std::string& s);
inline void out_mv(MValue* out, const MValue& v) { *out = v; }

// ---------------------------------------------------------------------------
// Event loop state (eventloop.cpp)
// ---------------------------------------------------------------------------

struct TimerJob {
    void (*f)(void*);
    void* env;
    void (*drop_fn)(void*);
};

struct TimerEntry {
    std::chrono::steady_clock::time_point due;
    uint64_t seq;
    TimerJob job;
};

// Min-heap ordering on (due, seq) — same as the Rust
// BinaryHeap<Reverse<TimerEntry>> with Ord by (due, seq).
struct TimerGreater {
    bool operator()(const TimerEntry& a, const TimerEntry& b) const {
        if (a.due != b.due) return a.due > b.due;
        return a.seq > b.seq;
    }
};

struct Rt {
    std::deque<std::function<void()>> tasks;
    size_t running = 0;
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerGreater> timers;
    uint64_t seq = 0;
    // Promises created but not yet resolved: the loop runs while > 0.
    size_t pending_promises = 0;
    // HTTP servers still listening — the program never exits while one
    // is up (stop with Ctrl-C).
    size_t pending_listeners = 0;
};

Rt& rt();
std::mutex& rt_mutex();
std::condition_variable& rt_cv();
bool quiescent(const Rt& g);

// ---------------------------------------------------------------------------
// Promise machinery (promise.cpp)
// ---------------------------------------------------------------------------

// (tag, payload) pairs retained on behalf of a background task; released
// when the task finishes.
using Owns = std::vector<std::pair<int32_t, void*>>;

// Retain `v`'s payload for a background task and record the release pair.
void own_into(Owns& owns, const MValue& v);
void drop_owns(const Owns& owns);

struct PromiseWork {
    Promise* promise;
    std::function<MValue()> work;
    Owns owns;
};

// A scheduled continuation: runs `resume(frame, state, &value)` on a
// worker thread, then drops the promise reference it owns.
struct WaiterTask {
    Promise* promise;
    Waiter waiter;
    MValue value;
};

void run_waiter(WaiterTask t);
void run_promise_work(PromiseWork w);

// Create a promise and enqueue `work` on the event loop; the work's
// returned MValue resolves the promise.
void* spawn_promise_io(std::function<MValue()> work, Owns owns);

// No-op resume used to hand a promise's final reference to the event
// loop (the waiter task releases it after running).
extern "C" void marina_noop_resume(void* frame, int32_t state, const MValue* value);

// ---------------------------------------------------------------------------
// Objects (object.cpp)
// ---------------------------------------------------------------------------

// marina_object_set + drop our own reference after storing.
void set_object_field(void* o, int64_t i, MValue v);
// Resolve a field index by name via the class registry (panics when
// unknown).
int64_t object_field_index(void* o, const std::string& name);

} // namespace marina_rt

#endif // MARINA_RUNTIME_INTERNAL_HPP