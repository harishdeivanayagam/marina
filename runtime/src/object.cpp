// marina runtime — objects & ghost classes: class metadata registry,
// field-indexed access, interface (name-based) access, console.log.
#include "runtime_internal.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace marina_rt {

// ---------------------------------------------------------------------------
// Class registry
// ---------------------------------------------------------------------------

std::mutex& class_mutex() {
    static std::mutex* m = new std::mutex(); // leaked intentionally
    return *m;
}

std::unordered_map<int32_t, ClassInfo>& class_map() {
    static std::unordered_map<int32_t, ClassInfo>* m =
        new std::unordered_map<int32_t, ClassInfo>(); // leaked intentionally
    return *m;
}

int64_t object_field_index(void* o, const std::string& name) {
    ObjectObj* obj = reinterpret_cast<ObjectObj*>(o);
    int32_t cid = obj->class_id;
    std::lock_guard<std::mutex> lk(class_mutex());
    auto it = class_map().find(cid);
    if (it == class_map().end()) {
        rt_panic("object of unknown class id " + std::to_string(cid) +
                 " (interface field access)");
    }
    const ClassInfo& info = it->second;
    for (size_t i = 0; i < info.fields.size(); i++) {
        if (info.fields[i] == name) return (int64_t)i;
    }
    rt_panic("class '" + info.name + "': no field named '" + name +
             "' (interface field access)");
}

void set_object_field(void* o, int64_t i, MValue v) {
    // object_set retains the payload; drop our own reference after storing
    marina_object_set(o, i, &v);
    drop_mv(v);
}

} // namespace marina_rt

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" void marina_class_register(int32_t class_id, const void* meta, int64_t len) {
    std::vector<uint8_t> bytes(reinterpret_cast<const uint8_t*>(meta),
                               reinterpret_cast<const uint8_t*>(meta) +
                                   (len > 0 ? (size_t)len : 0));
    std::vector<std::string> parts;
    {
        std::string cur;
        for (uint8_t b : bytes) {
            if (b == 0) {
                if (!cur.empty()) parts.push_back(cur);
                cur.clear();
            } else {
                cur += (char)b;
            }
        }
        if (!cur.empty()) parts.push_back(cur);
    }
    std::string name = parts.empty() ? "?" : marina_rt::utf8_lossy(
                                                 reinterpret_cast<const uint8_t*>(parts[0].data()),
                                                 parts[0].size());
    std::vector<std::string> fields;
    for (size_t i = 1; i < parts.size(); i++) {
        fields.push_back(marina_rt::utf8_lossy(
            reinterpret_cast<const uint8_t*>(parts[i].data()), parts[i].size()));
    }
    std::lock_guard<std::mutex> lk(marina_rt::class_mutex());
    marina_rt::class_map().insert_or_assign(
        class_id, marina_rt::ClassInfo{std::move(name), std::move(fields)});
}

extern "C" void* marina_object_new(int32_t class_id, int64_t n_fields) {
    marina_rt::ObjectObj* p = static_cast<marina_rt::ObjectObj*>(std::malloc(sizeof(marina_rt::ObjectObj)));
    if (!p) std::abort();
    // Uninitialized memory: placement-construct, never assign.
    new (&p->h.rc) std::atomic<int64_t>(1);
    p->h.kind = marina_rt::KIND_OBJECT;
    std::memset(p->h.pad, 0, sizeof p->h.pad);
    p->class_id = class_id;
    p->_pad = 0;
    new (&p->data) std::vector<MValue>(
        (size_t)(n_fields > 0 ? n_fields : 0), marina_rt::mv_void());
    return p;
}

static void object_bounds_panic(int64_t i, int64_t len) {
    marina_rt::rt_panic("field index " + std::to_string(i) +
                        " out of bounds (class has " + std::to_string(len) + " fields)");
}

extern "C" void marina_object_get(void* o, int64_t i, MValue* out) {
    marina_rt::ObjectObj* obj = reinterpret_cast<marina_rt::ObjectObj*>(o);
    int64_t len = (int64_t)obj->data.size();
    if (i < 0 || i >= len) object_bounds_panic(i, len);
    MValue v = obj->data[(size_t)i];
    marina_rt::retain_payload(v);
    *out = v;
}

extern "C" void marina_object_set(void* o, int64_t i, const MValue* v) {
    marina_rt::ObjectObj* obj = reinterpret_cast<marina_rt::ObjectObj*>(o);
    int64_t len = (int64_t)obj->data.size();
    if (i < 0 || i >= len) object_bounds_panic(i, len);
    marina_rt::retain_payload(*v);
    // store the MValue as-is: codegen builds it with the field's tag
    MValue new_v = *v;
    MValue old = obj->data[(size_t)i];
    obj->data[(size_t)i] = new_v;
    if (marina_rt::is_ptr_tag(old.tag) && old.p != nullptr) {
        marina_rt::release_ptr(old.p);
    }
}

extern "C" void marina_object_get_field(void* o, const MValue* name, MValue* out) {
    std::string n = marina_rt::mv_to_string(*name);
    int64_t i = marina_rt::object_field_index(o, n);
    marina_object_get(o, i, out);
}

extern "C" void marina_object_set_field(void* o, const MValue* name, const MValue* v) {
    std::string n = marina_rt::mv_to_string(*name);
    int64_t i = marina_rt::object_field_index(o, n);
    marina_object_set(o, i, v);
}

extern "C" int32_t marina_object_class_id(void* o) {
    if (!o) return -1;
    return reinterpret_cast<marina_rt::ObjectObj*>(o)->class_id;
}

extern "C" void marina_panic(const char* msg) {
    marina_rt::rt_panic(msg ? msg : "panic");
}

extern "C" void marina_print(const MValue* v) {
    std::cout << marina_rt::value_to_string(*v) << "\n";
}