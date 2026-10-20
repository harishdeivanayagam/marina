// marina runtime — core: ARC (retain/release/free), string objects,
// interned literals, value->string formatting, string primitives,
// frame/closure allocation, and small shared helpers.
#include "runtime_internal.hpp"

#include <cerrno>
#include <cmath>
#include <csetjmp>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <crt_externs.h>
#elif defined(__linux__)
#include <fstream>
#endif

namespace marina_rt {

// ---------------------------------------------------------------------------
// Panics & error rendering
// ---------------------------------------------------------------------------

// SJLJ-style exception frames: generated code pushes a frame (holding the
// setjmp buffer) with `marina_try_push` for each `try` block and pops it
// when the block exits. `rt_raise` longjmps into the innermost open frame
// on the current thread; with none open it aborts the process like before.
struct TryFrame {
    jmp_buf buf;
    TryFrame* prev;
};

thread_local TryFrame* try_top = nullptr;
// Message (and kind) of the exception in flight, read by the generated
// catch block through `marina_catch_string`.
thread_local std::string pending_error;
thread_local bool pending_is_panic = true;

[[noreturn]] void rt_die(const std::string& msg, bool is_panic) {
    std::cerr << (is_panic ? "marina runtime panic: " : "uncaught exception: ")
              << msg << "\n";
    std::exit(101);
}

[[noreturn]] void rt_raise(const std::string& msg, bool is_panic) {
    pending_error = msg;
    pending_is_panic = is_panic;
    TryFrame* top = try_top;
    if (top) {
        longjmp(top->buf, 1);
    }
    rt_die(msg, is_panic);
}

void rt_panic(const std::string& msg) {
    rt_raise(msg, true);
}

// Run a task (event-loop callback, waiter, timer job, http handler) on a
// worker thread. marina exceptions are longjmps into generated frames, so
// nothing normally escapes; any native C++ exception (bad_alloc, ...) is
// still reported and fatal instead of crashing with std::terminate.
void rt_guard(const std::function<void()>& f) {
    try {
        f();
    } catch (const std::exception& e) {
        rt_die(std::string("internal error: ") + e.what(), true);
    } catch (...) {
        rt_die("internal error (native exception)", true);
    }
}

std::string os_error(int err) {
    // Matches Rust std::io::Error Display for OS errors:
    // "<message> (os error <errno>)".
    const char* m = strerror(err); // strerror is not thread-safe on some
                                   // platforms; use the safe variant where
                                   // available.
#if defined(__APPLE__) || defined(__GLIBC__)
    char buf[256];
    if (strerror_r(err, buf, sizeof buf) == 0) m = buf;
#endif
    std::string s = m ? m : "";
    s += " (os error ";
    s += std::to_string(err);
    s += ")";
    return s;
}

int64_t sat_i64(double f) {
    // Rust `f64 as i64`: saturating, NaN -> 0.
    if (std::isnan(f)) return 0;
    if (f >= 9223372036854775807.0) return INT64_MAX;
    if (f <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)f;
}

bool parse_i64_strict(const std::string& s, int64_t* out) {
    // Rust str::parse::<i64>: optional +/- sign, digits, full consume,
    // range-checked.
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        i++;
    }
    if (i >= s.size()) return false;
    // Reject non-digit tails up front (digits only).
    unsigned long long acc = 0;
    bool overflow = false;
    for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        unsigned long long d = (unsigned long long)(s[i] - '0');
        if (acc > (1844674407370955161ULL)) overflow = true;
        else acc = acc * 10 + d;
        if (!overflow && acc > 9223372036854775807ULL + (neg ? 1 : 0)) overflow = true;
    }
    if (overflow) return false;
    if (neg) {
        *out = (acc == 9223372036854775808ULL) ? INT64_MIN : -(int64_t)acc;
    } else {
        *out = (int64_t)acc;
    }
    return true;
}

bool parse_usize_strict(const std::string& s, size_t* out) {
    // Rust str::parse::<usize>: accepts an optional leading '+', digits
    // only, full consume.
    size_t i = 0;
    if (i < s.size() && s[i] == '+') i++;
    if (i >= s.size()) return false;
    unsigned long long acc = 0;
    for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        unsigned long long d = (unsigned long long)(s[i] - '0');
        if (acc > (1844674407370955161ULL)) return false;
        acc = acc * 10 + d;
        if (acc > (unsigned long long)SIZE_MAX) return false;
    }
    *out = (size_t)acc;
    return true;
}

// ---------------------------------------------------------------------------
// UTF-8 lossy decoding (String::from_utf8_lossy)
// ---------------------------------------------------------------------------

std::string utf8_lossy(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n);
    size_t i = 0;
    static const char FFFD[] = "\xEF\xBF\xBD";
    while (i < n) {
        uint8_t b = p[i];
        if (b < 0x80) {
            out += (char)b;
            i++;
            continue;
        }
        size_t need = 0;
        uint8_t lo = 0x80, hi = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) need = 1;
        else if (b == 0xE0) { need = 2; lo = 0xA0; }
        else if ((b >= 0xE1 && b <= 0xEC) || (b >= 0xEE && b <= 0xEF)) need = 2;
        else if (b == 0xED) { need = 2; hi = 0x9F; }
        else if (b == 0xF0) { need = 3; lo = 0x90; }
        else if (b >= 0xF1 && b <= 0xF3) need = 3;
        else if (b == 0xF4) { need = 3; hi = 0x8F; }
        else {
            // Invalid lead byte: one replacement, skip one byte.
            out.append(FFFD, 3);
            i += 1;
            continue;
        }
        size_t k = 0;
        bool ran_out = false;
        size_t j = i + 1;
        for (; k < need; k++) {
            if (j >= n) { ran_out = true; break; }
            uint8_t c = p[j];
            uint8_t clo = (k == 0) ? lo : 0x80;
            uint8_t chi = (k == 0) ? hi : 0xBF;
            if (c < clo || c > chi) break;
            j++;
        }
        if (k == need) {
            out.append(reinterpret_cast<const char*>(p) + i, need + 1);
            i = j;
        } else if (ran_out) {
            // Incomplete sequence at end of input: consume the rest.
            out.append(FFFD, 3);
            i = n;
        } else {
            // Malformed sequence: consume lead + the continuations that
            // were part of the attempted sequence.
            out.append(FFFD, 3);
            i = j;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// ARC
// ---------------------------------------------------------------------------

static void* c_alloc(size_t size) {
    // Opaque malloc/free helpers for compiler-allocated memory (async
    // frames, closure environments). The total size is stored in an
    // 8-byte header so we can free without the caller remembering it.
    size_t total = size + 8;
    uint8_t* p = static_cast<uint8_t*>(std::malloc(total));
    if (!p) std::abort();
    *reinterpret_cast<size_t*>(p) = total;
    return p + 8;
}

static void c_free(void* p) {
    if (p == nullptr) return;
    uint8_t* q = static_cast<uint8_t*>(p) - 8;
    std::free(q);
}

void* alloc_string(const uint8_t* bytes, size_t len, int64_t rc) {
    uint8_t* p = static_cast<uint8_t*>(std::malloc(STR_HDR + len));
    if (!p) std::abort();
    StrObj* o = reinterpret_cast<StrObj*>(p);
    new (&o->h.rc) std::atomic<int64_t>(rc);
    o->h.kind = KIND_STRING;
    std::memset(o->h.pad, 0, sizeof o->h.pad);
    o->len = len;
    if (len) std::memcpy(p + STR_HDR, bytes, len);
    return p;
}

MValue mv_str_owned(const std::string& s) {
    void* p = alloc_string(s, 1);
    return MValue{TAG_STRING, 0, 0, 0.0, p};
}

void retain_ptr(void* p) {
    Header* h = reinterpret_cast<Header*>(p);
    int64_t rc = h->rc.load(std::memory_order_relaxed);
    if (rc < 0) return; // immortal
    h->rc.fetch_add(1, std::memory_order_relaxed);
}

void release_ptr(void* p) {
    Header* h = reinterpret_cast<Header*>(p);
    int64_t rc = h->rc.load(std::memory_order_relaxed);
    if (rc < 0) return; // immortal
    if (h->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) free_obj(p);
}

void free_obj(void* p) {
    Header* h = reinterpret_cast<Header*>(p);
    switch (h->kind) {
        case KIND_STRING: {
            // StrObj is trivially destructible beyond the header.
            std::free(p);
            break;
        }
        case KIND_PROMISE: {
            Promise* pr = reinterpret_cast<Promise*>(p);
            release_value(pr->inner.value);
            std::destroy_at(&pr->inner); // drops waiters + the mutex
            std::free(p);
            break;
        }
        case KIND_ARRAY: {
            ArrayObj* arr = reinterpret_cast<ArrayObj*>(p);
            release_elements(arr->data);
            std::destroy_at(&arr->data); // release the vector's buffer too
            std::free(p);
            break;
        }
        case KIND_OBJECT: {
            ObjectObj* obj = reinterpret_cast<ObjectObj*>(p);
            release_elements(obj->data);
            std::destroy_at(&obj->data);
            std::free(p);
            break;
        }
        default:
            break;
    }
}

void retain_value(const MValue& v) {
    if (owns_ptr_tag(v.tag) && v.p != nullptr) retain_ptr(v.p);
}

void release_value(const MValue& v) {
    if (owns_ptr_tag(v.tag) && v.p != nullptr) release_ptr(v.p);
}

void retain_payload(const MValue& v) {
    if (is_ptr_tag(v.tag) && v.p != nullptr) {
        Header* h = reinterpret_cast<Header*>(v.p);
        if (h->rc.load(std::memory_order_relaxed) >= 0) retain_ptr(v.p);
    }
}

void release_elements(const std::vector<MValue>& vs) {
    for (const MValue& v : vs) {
        if (is_ptr_tag(v.tag) && v.p != nullptr) release_ptr(v.p);
    }
}

// Interned string literals (immortal, created on first use).
extern "C" void* marina_string_literal(const uint8_t* bytes, int64_t len) {
    if (len < 0) len = 0;
    std::string key(reinterpret_cast<const char*>(bytes), (size_t)len);
    struct Table {
        std::mutex mu;
        std::unordered_map<std::string, void*> map;
    };
    static Table* t = new Table(); // leaked intentionally
    std::lock_guard<std::mutex> lk(t->mu);
    auto it = t->map.find(key);
    if (it != t->map.end()) return it->second;
    void* p = alloc_string(bytes, (size_t)len, -1); // immortal
    t->map.emplace(std::move(key), p);
    return p;
}

// ---------------------------------------------------------------------------
// Frame / closure allocation
// ---------------------------------------------------------------------------

extern "C" void* marina_frame_alloc(int64_t size) { return c_alloc((size_t)(size > 0 ? size : 0)); }
extern "C" void marina_frame_free(void* p) { c_free(p); }
extern "C" void* marina_closure_alloc(int64_t size) { return c_alloc((size_t)(size > 0 ? size : 0)); }
extern "C" void marina_closure_free(void* p) { c_free(p); }

extern "C" void* marina_retain(void* p) {
    retain_ptr(p);
    return p;
}

extern "C" void marina_release(void* p) { release_ptr(p); }

// ---------------------------------------------------------------------------
// Value -> string (Rust `{}` Display semantics)
// ---------------------------------------------------------------------------

std::string fmt_float(double f) {
    if (std::isfinite(f) && f == std::trunc(f) && std::fabs(f) < 1e15) {
        return std::to_string(sat_i64(f));
    }
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f > 0 ? "inf" : "-inf";
    // Rust's f64 Display prints the shortest round-trip decimal digits in
    // positional (never scientific) notation. Find the shortest precision
    // whose correctly-rounded %e representation round-trips, strip
    // trailing zeros, and render positionally.
    char buf[64];
    int prec = 0;
    for (; prec <= 16; prec++) {
        std::snprintf(buf, sizeof buf, "%.*e", prec, f);
        if (std::strtod(buf, nullptr) == f) break;
    }
    if (prec > 16) std::snprintf(buf, sizeof buf, "%.17e", f);
    // Parse "<sign><d>[.ddd]e<exp>".
    const char* s = buf;
    bool neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    std::string digits;
    digits += *s++;
    if (*s == '.') {
        s++;
        while (*s && *s != 'e') digits += *s++;
    }
    int exp10 = std::atoi(s + 1); // after 'e'
    // Strip trailing zeros (they don't change the value).
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    size_t n = digits.size();
    std::string out;
    if (neg) out += '-';
    if (exp10 >= (int)n - 1) {
        out += digits;
        for (int z = (int)n - 1; z < exp10; z++) out += '0';
    } else if (exp10 >= 0) {
        out += digits.substr(0, (size_t)exp10 + 1);
        out += '.';
        out += digits.substr((size_t)exp10 + 1);
    } else {
        out += "0.";
        for (int z = 0; z < -exp10 - 1; z++) out += '0';
        out += digits;
    }
    return out;
}

static std::string join_with(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string value_to_string(const MValue& v) {
    switch (v.tag) {
        case TAG_STRING:
            return utf8_lossy(str_bytes(v.p), (size_t)str_len(v.p));
        case TAG_INT:
            return fmt_tagged_int(v);
        case TAG_FLOAT:
            return fmt_tagged_float(v);
        case TAG_BOOL:
            return v.i != 0 ? "true" : "false";
        case TAG_STREAM:
            return "[stream " + std::to_string(v.i) + "]";
        case TAG_ARRAY: {
            const ArrayObj* arr = reinterpret_cast<const ArrayObj*>(v.p);
            std::vector<std::string> parts;
            parts.reserve(arr->data.size());
            for (const MValue& e : arr->data) parts.push_back(value_to_string(e));
            return "[" + join_with(parts, ", ") + "]";
        }
        case TAG_OBJECT: {
            const ObjectObj* obj = reinterpret_cast<const ObjectObj*>(v.p);
            std::lock_guard<std::mutex> lk(class_mutex());
            auto it = class_map().find(obj->class_id);
            if (it == class_map().end()) return "{...}";
            const ClassInfo& ci = it->second;
            std::vector<std::string> parts;
            parts.reserve(obj->data.size());
            for (size_t i = 0; i < obj->data.size(); i++) {
                std::string name = i < ci.fields.size() ? ci.fields[i] : "?";
                parts.push_back(name + ": " + value_to_string(obj->data[i]));
            }
            return ci.name + " { " + join_with(parts, ", ") + " }";
        }
        default:
            return "undefined";
    }
}

std::string mv_to_string(const MValue& v) {
    if (v.tag == TAG_STRING) {
        return utf8_lossy(str_bytes(v.p), (size_t)str_len(v.p));
    }
    return value_to_string(v);
}

extern "C" void* marina_to_string(const MValue* v) {
    if (v->tag == TAG_STRING) {
        uint8_t* p = static_cast<uint8_t*>(v->p);
        if (p != nullptr) {
            Header* h = reinterpret_cast<Header*>(p);
            if (h->rc.load(std::memory_order_relaxed) >= 0) retain_ptr(p);
            return p;
        }
        return alloc_string(std::string(), 1);
    }
    return alloc_string(value_to_string(*v), 1);
}

// ---------------------------------------------------------------------------
// String primitives
// ---------------------------------------------------------------------------

extern "C" void* marina_string_concat(const void* a, const void* b) {
    size_t la = str_len(const_cast<void*>(a));
    size_t lb = str_len(const_cast<void*>(b));
    std::string out;
    out.reserve(la + lb);
    out.append(reinterpret_cast<const char*>(str_bytes(const_cast<void*>(a))), la);
    out.append(reinterpret_cast<const char*>(str_bytes(const_cast<void*>(b))), lb);
    return alloc_string(out, 1);
}

extern "C" int64_t marina_string_len(const void* s) {
    return (int64_t)str_len(const_cast<void*>(s));
}

extern "C" int64_t marina_string_charcode_at(const void* s, int64_t i) {
    size_t len = str_len(const_cast<void*>(s));
    if (i < 0 || (size_t)i >= len) return 0;
    return (int64_t)str_bytes(const_cast<void*>(s))[i];
}

extern "C" void* marina_string_substring(const void* s, int64_t start, int64_t end) {
    size_t len = str_len(const_cast<void*>(s));
    int64_t l = (int64_t)len;
    int64_t a = start < 0 ? 0 : (start > l ? l : start);
    int64_t b = end < 0 ? 0 : (end > l ? l : end);
    if (b < a) b = a;
    return alloc_string(str_bytes(const_cast<void*>(s)) + a,
                        (size_t)(b - a), 1);
}

extern "C" bool marina_str_eq(const MValue* a, const MValue* b) {
    return mv_to_string(*a) == mv_to_string(*b);
}

// ---------------------------------------------------------------------------
// Process arguments (fs.args())
// ---------------------------------------------------------------------------

static std::vector<std::string> process_args() {
    std::vector<std::string> out;
#if defined(__APPLE__)
    int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    for (int i = 0; i < argc; i++) {
        if (argv[i]) out.emplace_back(argv[i]);
    }
#elif defined(__linux__)
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    std::string tok;
    while (std::getline(f, tok, '\0')) out.push_back(tok);
#endif
    return out;
}

extern "C" void* marina_args() {
    void* arr = marina_array_new(TAG_ARRAY);
    ArrayObj* a = reinterpret_cast<ArrayObj*>(arr);
    for (const std::string& arg : process_args()) {
        MValue mv{};
        mv.tag = TAG_STRING;
        mv.p = alloc_string(arg, 1);
        a->data.push_back(mv);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// try / catch / throw (SJLJ-style exception frames)
// ---------------------------------------------------------------------------

// Open a handler frame and return the setjmp buffer for the generated code
// (`call i32 @setjmp(ptr buf)` — declared returns_twice). The frame stays
// open until marina_try_pop or a longjmp lands in it.
extern "C" void* marina_try_push() {
    TryFrame* f = new TryFrame();
    f->prev = try_top;
    try_top = f;
    return &f->buf;
}

extern "C" void marina_try_pop() {
    TryFrame* top = try_top;
    if (top) {
        try_top = top->prev;
        delete top;
    }
}

// The catch block's string binding: a fresh owned marina string holding the
// message of the exception that was just caught.
extern "C" void* marina_catch_string() {
    return alloc_string(pending_error, 1);
}

// `throw expr;` — expr is a string value (checked by the compiler).
extern "C" void marina_throw_str(const MValue* v) {
    rt_raise(mv_to_string(*v), false);
}

} // namespace marina_rt