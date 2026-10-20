// marina runtime — arrays: growable vector of tagged MValues with ARC
// element ownership, plus the synchronous higher-order methods.
#include "runtime_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace marina_rt {

namespace {

MValue mv_from(int32_t tag, const MValue& v) {
    MValue m = v;
    m.tag = tag;
    // pad carries integer signedness / float width for printing and sort
    return m;
}

ArrayObj* as_array(void* p) { return reinterpret_cast<ArrayObj*>(p); }

void array_get_checked_panic(int64_t i, int64_t len) {
    std::cerr << "marina runtime panic: array index " << i
              << " out of bounds (length " << len << ")\n";
    std::exit(101);
}

MValue array_get_checked(void* a, int64_t i) {
    ArrayObj* arr = as_array(a);
    int64_t len = (int64_t)arr->data.size();
    if (i < 0 || i >= len) array_get_checked_panic(i, len);
    MValue v = arr->data[(size_t)i];
    retain_payload(v);
    return v;
}

// Comparison for sort: strings bytewise, numbers numerically (NaN
// compares Equal, like Rust's partial_cmp(...).unwrap_or(Equal)).
int mv_cmp(const MValue& x, const MValue& y) {
    if (x.tag == TAG_STRING && y.tag == TAG_STRING) {
        void* xp = x.p;
        void* yp = y.p;
        size_t lx = (size_t)str_len(xp);
        size_t ly = (size_t)str_len(yp);
        size_t m = lx < ly ? lx : ly;
        int c = m ? std::memcmp(str_bytes(xp), str_bytes(yp), m) : 0;
        if (c != 0) return c < 0 ? -1 : 1;
        return lx < ly ? -1 : (lx > ly ? 1 : 0);
    }
    if (x.tag == TAG_INT && y.tag == TAG_INT) {
        bool unsign = ((x.pad | y.pad) & 0x100) != 0;
        if (unsign) {
            uint64_t xi = static_cast<uint64_t>(x.i);
            uint64_t yi = static_cast<uint64_t>(y.i);
            if (xi < yi) return -1;
            if (xi > yi) return 1;
            return 0;
        }
        if (x.i < y.i) return -1;
        if (x.i > y.i) return 1;
        return 0;
    }
    double fx = x.tag == TAG_FLOAT ? (x.pad == 32 ? (double)(float)x.f : x.f) : (double)x.i;
    double fy = y.tag == TAG_FLOAT ? (y.pad == 32 ? (double)(float)y.f : y.f) : (double)y.i;
    if (fx < fy) return -1;
    if (fx > fy) return 1;
    return 0;
}

} // namespace

} // namespace marina_rt

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" void* marina_array_new(int32_t tag) {
    marina_rt::ArrayObj* p = static_cast<marina_rt::ArrayObj*>(std::malloc(sizeof(marina_rt::ArrayObj)));
    if (!p) std::abort();
    // Uninitialized memory: placement-construct, never assign.
    new (&p->h.rc) std::atomic<int64_t>(1);
    p->h.kind = marina_rt::KIND_ARRAY;
    std::memset(p->h.pad, 0, sizeof p->h.pad);
    p->tag = tag;
    p->_pad = 0;
    new (&p->data) std::vector<MValue>();
    return p;
}

extern "C" int64_t marina_array_len(void* a) {
    return (int64_t)reinterpret_cast<marina_rt::ArrayObj*>(a)->data.size();
}

extern "C" int64_t marina_array_push(void* a, const MValue* v) {
    marina_rt::ArrayObj* arr = reinterpret_cast<marina_rt::ArrayObj*>(a);
    marina_rt::retain_payload(*v);
    arr->data.push_back(marina_rt::mv_from(arr->tag, *v));
    return (int64_t)arr->data.size();
}

extern "C" void marina_array_pop(void* a, MValue* out) {
    marina_rt::ArrayObj* arr = reinterpret_cast<marina_rt::ArrayObj*>(a);
    if (arr->data.empty()) {
        std::cerr << "marina runtime panic: pop from an empty array\n";
        std::exit(101);
    }
    MValue v = arr->data.back();
    arr->data.pop_back();
    marina_rt::retain_payload(v);
    *out = v;
}

extern "C" void marina_array_shift(void* a, MValue* out) {
    marina_rt::ArrayObj* arr = reinterpret_cast<marina_rt::ArrayObj*>(a);
    if (arr->data.empty()) {
        std::cerr << "marina runtime panic: shift from an empty array\n";
        std::exit(101);
    }
    MValue v = arr->data.front();
    arr->data.erase(arr->data.begin());
    marina_rt::retain_payload(v);
    *out = v;
}

extern "C" int64_t marina_array_unshift(void* a, const MValue* v) {
    marina_rt::ArrayObj* arr = reinterpret_cast<marina_rt::ArrayObj*>(a);
    marina_rt::retain_payload(*v);
    arr->data.insert(arr->data.begin(), marina_rt::mv_from(arr->tag, *v));
    return (int64_t)arr->data.size();
}

extern "C" void marina_array_get(void* a, int64_t i, MValue* out) {
    MValue v = marina_rt::array_get_checked(a, i);
    *out = v;
}

extern "C" void marina_array_set(void* a, int64_t i, const MValue* v) {
    marina_rt::ArrayObj* arr = reinterpret_cast<marina_rt::ArrayObj*>(a);
    int64_t len = (int64_t)arr->data.size();
    if (i < 0 || i >= len) {
        std::cerr << "marina runtime panic: array index " << i
                  << " out of bounds (length " << len << ")\n";
        std::exit(101);
    }
    MValue nv = marina_rt::mv_from(arr->tag, *v);
    marina_rt::retain_payload(nv);
    MValue old = arr->data[(size_t)i];
    arr->data[(size_t)i] = nv;
    if (marina_rt::is_ptr_tag(old.tag) && old.p != nullptr) {
        marina_rt::release_ptr(old.p);
    }
}

extern "C" void* marina_array_splice(void* a, int64_t start, int64_t count) {
    marina_rt::ArrayObj* arr = reinterpret_cast<marina_rt::ArrayObj*>(a);
    int64_t len = (int64_t)arr->data.size();
    int64_t s = start < 0 ? 0 : (start > len ? len : start);
    int64_t c = count < 0 ? 0 : (count > len ? len : count);
    int64_t e = s + c;
    if (e > len) e = len;
    // Ownership of the removed elements moves into the new array.
    std::vector<MValue> removed(arr->data.begin() + s, arr->data.begin() + e);
    arr->data.erase(arr->data.begin() + s, arr->data.begin() + e);
    void* out = marina_array_new(arr->tag);
    reinterpret_cast<marina_rt::ArrayObj*>(out)->data = std::move(removed);
    return out;
}

extern "C" void* marina_array_slice(void* a, int64_t start, int64_t end) {
    marina_rt::ArrayObj* arr = reinterpret_cast<marina_rt::ArrayObj*>(a);
    int64_t len = (int64_t)arr->data.size();
    int64_t s = start < 0 ? 0 : (start > len ? len : start);
    int64_t e = end < 0 ? 0 : (end > len ? len : end);
    if (s > e) {
        int64_t tmp = s;
        s = e;
        e = tmp;
    }
    void* out = marina_array_new(arr->tag);
    marina_rt::ArrayObj* dst = reinterpret_cast<marina_rt::ArrayObj*>(out);
    for (int64_t i = s; i < e; i++) {
        marina_rt::retain_payload(arr->data[(size_t)i]);
        dst->data.push_back(arr->data[(size_t)i]);
    }
    return out;
}

extern "C" void* marina_array_concat(void* a, void* b) {
    void* out = marina_array_new(reinterpret_cast<marina_rt::ArrayObj*>(a)->tag);
    marina_rt::ArrayObj* dst = reinterpret_cast<marina_rt::ArrayObj*>(out);
    for (void* src : {a, b}) {
        for (const MValue& v :
             reinterpret_cast<marina_rt::ArrayObj*>(src)->data) {
            marina_rt::retain_payload(v);
            dst->data.push_back(v);
        }
    }
    return out;
}

extern "C" void* marina_array_reverse(void* a) {
    auto& data = reinterpret_cast<marina_rt::ArrayObj*>(a)->data;
    std::reverse(data.begin(), data.end());
    marina_rt::retain_ptr(a);
    return a;
}

extern "C" void* marina_array_sort(void* a) {
    auto& data = reinterpret_cast<marina_rt::ArrayObj*>(a)->data;
    // Rust's sort_by is stable.
    std::stable_sort(data.begin(), data.end(),
                     [](const MValue& x, const MValue& y) {
                         return marina_rt::mv_cmp(x, y) < 0;
                     });
    marina_rt::retain_ptr(a);
    return a;
}

extern "C" void* marina_array_join(void* a, const void* sep) {
    marina_rt::ArrayObj* arr = reinterpret_cast<marina_rt::ArrayObj*>(a);
    std::string sep_s = marina_rt::utf8_lossy(
        marina_rt::str_bytes(const_cast<void*>(sep)),
        (size_t)marina_rt::str_len(const_cast<void*>(sep)));
    std::string out;
    for (size_t i = 0; i < arr->data.size(); i++) {
        if (i > 0) out += sep_s;
        out += marina_rt::value_to_string(arr->data[i]);
    }
    return marina_rt::alloc_string(out, 1);
}

// --- higher-order methods (callbacks run synchronously on the caller) ---

extern "C" void marina_array_foreach(void* a, const void* cb, void* env, const void* env_drop) {
    auto f = reinterpret_cast<MarinaArrayCb1>(const_cast<void*>(cb));
    auto d = reinterpret_cast<MarinaFn0>(const_cast<void*>(env_drop));
    // Iterate a snapshot, like the Rust `data.clone()`.
    std::vector<MValue> snapshot =
        reinterpret_cast<marina_rt::ArrayObj*>(a)->data;
    for (const MValue& v : snapshot) {
        marina_rt::retain_payload(v);
        f(env, &v);
    }
    d(env);
}

extern "C" void* marina_array_map(void* a, const void* cb, void* env, const void* env_drop,
                       int32_t out_tag) {
    auto f = reinterpret_cast<MarinaArrayCb2>(const_cast<void*>(cb));
    auto d = reinterpret_cast<MarinaFn0>(const_cast<void*>(env_drop));
    void* out = marina_array_new(out_tag);
    marina_rt::ArrayObj* dst = reinterpret_cast<marina_rt::ArrayObj*>(out);
    std::vector<MValue> snapshot =
        reinterpret_cast<marina_rt::ArrayObj*>(a)->data;
    for (const MValue& v : snapshot) {
        marina_rt::retain_payload(v);
        MValue res = marina_rt::mv_void();
        f(env, &v, &res);
        marina_rt::retain_payload(res);
        dst->data.push_back(marina_rt::mv_from(out_tag, res));
    }
    d(env);
    return out;
}

extern "C" void* marina_array_filter(void* a, const void* cb, void* env, const void* env_drop) {
    auto f = reinterpret_cast<MarinaArrayCb2>(const_cast<void*>(cb));
    auto d = reinterpret_cast<MarinaFn0>(const_cast<void*>(env_drop));
    marina_rt::ArrayObj* src = reinterpret_cast<marina_rt::ArrayObj*>(a);
    void* out = marina_array_new(src->tag);
    marina_rt::ArrayObj* dst = reinterpret_cast<marina_rt::ArrayObj*>(out);
    std::vector<MValue> snapshot = src->data;
    for (const MValue& v : snapshot) {
        marina_rt::retain_payload(v);
        MValue res = marina_rt::mv_void();
        f(env, &v, &res);
        if (res.tag == TAG_BOOL && res.i != 0) {
            marina_rt::retain_payload(v);
            dst->data.push_back(v);
        }
    }
    d(env);
    return out;
}

extern "C" void marina_array_reduce(void* a, const void* cb, void* env, const void* env_drop,
                         const MValue* init, MValue* out) {
    auto f = reinterpret_cast<MarinaArrayCb3>(const_cast<void*>(cb));
    auto d = reinterpret_cast<MarinaFn0>(const_cast<void*>(env_drop));
    std::vector<MValue> snapshot =
        reinterpret_cast<marina_rt::ArrayObj*>(a)->data;
    MValue acc = *init;
    marina_rt::retain_payload(acc);
    for (const MValue& v : snapshot) {
        marina_rt::retain_payload(v);
        MValue res = marina_rt::mv_void();
        f(env, &acc, &v, &res);
        marina_rt::retain_payload(res);
        // release the previous accumulator's payload
        if (marina_rt::is_ptr_tag(acc.tag) && acc.p != nullptr) {
            marina_rt::release_ptr(acc.p);
        }
        acc = res;
    }
    marina_rt::retain_payload(acc);
    *out = acc;
    d(env);
}