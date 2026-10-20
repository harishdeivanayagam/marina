// marina runtime — event loop: worker pool, timer thread (binary
// min-heap of timers ordered by (due, seq)), quiescence detection.
#include "runtime_internal.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace marina_rt {

// The global loop state. Heap-allocated (and intentionally leaked) so
// the worker/timer threads never race static destructors at exit.
Rt& rt() {
    static Rt* r = new Rt();
    return *r;
}

std::mutex& rt_mutex() {
    static std::mutex* m = new std::mutex();
    return *m;
}

std::condition_variable& rt_cv() {
    static std::condition_variable* c = new std::condition_variable();
    return *c;
}

bool quiescent(const Rt& g) {
    return g.tasks.empty() && g.running == 0 && g.timers.empty() &&
           g.pending_promises == 0 && g.pending_listeners == 0;
}

static void worker_loop() {
    std::mutex& m = rt_mutex();
    std::condition_variable& cv = rt_cv();
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m);
            for (;;) {
                Rt& g = rt();
                if (!g.tasks.empty()) {
                    task = std::move(g.tasks.front());
                    g.tasks.pop_front();
                    g.running += 1;
                    break;
                }
                cv.wait(lk);
            }
        }
        rt_guard(task);
        {
            std::lock_guard<std::mutex> lk(m);
            rt().running -= 1;
            cv.notify_all();
        }
    }
}

static void timer_loop() {
    std::mutex& m = rt_mutex();
    std::condition_variable& cv = rt_cv();
    std::unique_lock<std::mutex> lk(m);
    for (;;) {
        Rt& g = rt();
        if (!g.timers.empty()) {
            auto due = g.timers.top().due;
            auto now = std::chrono::steady_clock::now();
            if (now >= due) {
                // Pop the timer and schedule its callback as a task in
                // the same critical section so the loop is never
                // momentarily "quiescent".
                TimerEntry entry = g.timers.top();
                g.timers.pop();
                TimerJob job = entry.job;
                g.tasks.push_back([job] {
                    job.f(job.env);
                    job.drop_fn(job.env);
                });
                cv.notify_all();
                continue;
            }
            cv.wait_until(lk, due);
        } else {
            cv.wait(lk);
        }
    }
}

} // namespace marina_rt

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" void marina_event_loop_start() {
    static std::atomic<bool> started{false};
    if (started.exchange(true, std::memory_order_seq_cst)) return;
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    if (n > 8) n = 8;
    for (unsigned i = 0; i < n; i++) std::thread(marina_rt::worker_loop).detach();
    std::thread(marina_rt::timer_loop).detach();
}

extern "C" int32_t marina_event_loop_run() {
    std::unique_lock<std::mutex> lk(marina_rt::rt_mutex());
    while (!marina_rt::quiescent(marina_rt::rt())) {
        marina_rt::rt_cv().wait(lk);
    }
    return 0;
}

extern "C" void marina_set_timeout(const void* f, void* env, const void* env_drop, int64_t ms) {
    void (*fp)(void*) = reinterpret_cast<void (*)(void*)>(const_cast<void*>(f));
    void (*dp)(void*) = reinterpret_cast<void (*)(void*)>(const_cast<void*>(env_drop));
    std::lock_guard<std::mutex> lk(marina_rt::rt_mutex());
    marina_rt::Rt& g = marina_rt::rt();
    uint64_t seq = g.seq + 1;
    g.seq = seq;
    g.timers.push(marina_rt::TimerEntry{
        std::chrono::steady_clock::now() +
            std::chrono::milliseconds(ms > 0 ? ms : 0),
        seq,
        marina_rt::TimerJob{fp, env, dp},
    });
    marina_rt::rt_cv().notify_all();
}