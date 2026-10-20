// marina runtime — promises: create / run_executor / resolve / await,
// plus the promise-returning wrappers that run blocking I/O work on the
// worker pool (spawn_promise_io).
#include "runtime_internal.hpp"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace marina_rt {

// ---------------------------------------------------------------------------
// Background-task ownership helpers
// ---------------------------------------------------------------------------

void own_into(Owns& owns, const MValue& v) {
    if (owns_ptr_tag(v.tag) && v.p != nullptr) {
        retain_ptr(v.p);
        owns.emplace_back(v.tag, v.p);
    }
}

void drop_owns(const Owns& owns) {
    for (const auto& kv : owns) {
        if (owns_ptr_tag(kv.first) && kv.second != nullptr) release_ptr(kv.second);
    }
}

// ---------------------------------------------------------------------------
// Scheduled continuations
// ---------------------------------------------------------------------------

void run_waiter(WaiterTask t) {
    t.waiter.resume(t.waiter.frame, t.waiter.state, &t.value);
    release_ptr(t.promise);
    // The waiter task owned a copy of the resolved value (retained by
    // marina_promise_resolve / the marina_await fast path). The resumed
    // frame has taken over ownership (or retained its own), so drop ours.
    release_value(t.value);
}

void run_promise_work(PromiseWork w) {
    MValue v = w.work();
    marina_promise_resolve(w.promise, &v);
    drop_owns(w.owns);
    release_ptr(w.promise);
}

void* spawn_promise_io(std::function<MValue()> work, Owns owns) {
    void* p = marina_promise_create();
    retain_ptr(p); // the task's reference
    PromiseWork w{reinterpret_cast<Promise*>(p), std::move(work), std::move(owns)};
    {
        std::lock_guard<std::mutex> lk(rt_mutex());
        rt().tasks.push_back([w] { run_promise_work(w); });
        rt_cv().notify_all();
    }
    return p;
}

// No-op resume used to hand a promise's final reference to the event
// loop (the waiter task releases it after running).
extern "C" void marina_noop_resume(void*, int32_t, const MValue*) {}

} // namespace marina_rt

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" void* marina_promise_create() {
    marina_rt::Promise* p = static_cast<marina_rt::Promise*>(std::malloc(sizeof(marina_rt::Promise)));
    if (!p) std::abort();
    // Uninitialized memory: placement-construct every non-trivial field.
    new (&p->h.rc) std::atomic<int64_t>(1);
    p->h.kind = marina_rt::KIND_PROMISE;
    std::memset(p->h.pad, 0, sizeof p->h.pad);
    new (&p->inner) marina_rt::PromiseInner();
    {
        std::lock_guard<std::mutex> lk(marina_rt::rt_mutex());
        marina_rt::rt().pending_promises += 1;
    }
    return p;
}

extern "C" void* marina_promise_run_executor(void* promise, const void* executor, void* env,
                                  const void* env_drop) {
    void (*f)(void*) = reinterpret_cast<void (*)(void*)>(const_cast<void*>(executor));
    void (*d)(void*) = reinterpret_cast<void (*)(void*)>(const_cast<void*>(env_drop));
    f(env);
    d(env);
    marina_rt::retain_ptr(promise);
    return promise;
}

extern "C" void marina_promise_resolve(void* promise, const MValue* value) {
    marina_rt::Promise* p = static_cast<marina_rt::Promise*>(promise);
    std::vector<marina_rt::Waiter> waiters;
    MValue v;
    {
        std::lock_guard<std::mutex> lk(p->inner.mu);
        if (p->inner.done) return;
        v = *value; // TRANSFERS the task's ref to the promise
        p->inner.value = v;
        p->inner.done = true;
        waiters = std::move(p->inner.waiters);
        p->inner.waiters.clear();
    }
    {
        std::lock_guard<std::mutex> lk(marina_rt::rt_mutex());
        marina_rt::Rt& g = marina_rt::rt();
        for (marina_rt::Waiter& w : waiters) {
            if (!w.owns_promise_ref) {
                marina_rt::retain_ptr(promise); // waiter task owns a ref
            }
            // (if the waiter owns the caller's ref, the task inherits it)
            marina_rt::retain_value(v); // the waiter task owns a copy of the value
            marina_rt::WaiterTask t{p, w, v};
            g.tasks.push_back([t] { marina_rt::run_waiter(t); });
        }
        g.pending_promises -= 1;
        marina_rt::rt_cv().notify_all();
    }
}

extern "C" void marina_await(void* promise, const void* resume, void* frame, int32_t state) {
    marina_rt::Promise* p = static_cast<marina_rt::Promise*>(promise);
    marina_rt::ResumeFn f = reinterpret_cast<marina_rt::ResumeFn>(const_cast<void*>(resume));
    {
        std::lock_guard<std::mutex> lk(p->inner.mu);
        if (!p->inner.done) {
            // The await consumes the caller's reference to the promise:
            // the waiter task inherits it and releases it after resume.
            p->inner.waiters.push_back(marina_rt::Waiter{f, frame, state, true});
            return;
        }
    }
    // Done already: run the resume through a waiter task with the same
    // semantics as a delayed resolution.
    MValue value = p->inner.value;
    // The await consumes the caller's promise reference: hand it to the
    // waiter task (run_waiter releases it after resume).
    marina_rt::retain_value(value);
    marina_rt::WaiterTask t{p, marina_rt::Waiter{f, frame, state, true}, value};
    {
        std::lock_guard<std::mutex> lk(marina_rt::rt_mutex());
        marina_rt::rt().tasks.push_back([t] { marina_rt::run_waiter(t); });
        marina_rt::rt_cv().notify_all();
    }
}