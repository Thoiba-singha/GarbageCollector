#include "gc.h"

#include <algorithm>   // std::stable_partition
#include <cstdlib>     // std::atexit
#include <mutex>


namespace GC {

    using namespace std;

    // ─────────────────────────────────────────────────────────────────────────────
    // Global state
    // ─────────────────────────────────────────────────────────────────────────────

    std::mutex                   gc_mutex;
    thread_local gc_object* current = nullptr;
    std::vector<gc_object*>      all_objects;
    std::atomic<long>            gc_counter{ 1024 };

    // ─────────────────────────────────────────────────────────────────────────────
    // Automatic collection at program exit
    // ─────────────────────────────────────────────────────────────────────────────

    namespace {

        void gc_atexit_handler() noexcept
        {
            gc_collect();
        }

        // Self-registering struct: its constructor runs before main() and installs
        // the atexit handler exactly once (ODR-safe because it lives in this TU).
        struct GcAtExitRegistrar {
            GcAtExitRegistrar() noexcept { std::atexit(gc_atexit_handler); }
        } const gc_atexit_registrar;

    } // anonymous namespace

    // ─────────────────────────────────────────────────────────────────────────────
    // gc_object
    // ─────────────────────────────────────────────────────────────────────────────

    gc_object::gc_object() noexcept
        : end_(this + 1)
        , destructor([](std::byte*, std::byte*) noexcept {})
    {
        root_ref_cnt.store(0, std::memory_order_relaxed);
        first.store(nullptr, std::memory_order_relaxed);
    }

    gc_object::gc_object(void* e, void (*d)(std::byte*, std::byte*) noexcept) noexcept
        : end_(e)
        , destructor(d)
    {
        root_ref_cnt.store(0, std::memory_order_relaxed);
        first.store(nullptr, std::memory_order_relaxed);
    }

    gc_object::~gc_object()
    {
        destructor(static_cast<std::byte*>(start()),
            static_cast<std::byte*>(end()));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // gc_base_ptr – reference-count helpers
    // ─────────────────────────────────────────────────────────────────────────────

    // Increment root_ref_cnt.  Requires the lock only when going 0 → 1.
    void gc_base_ptr::inc_root(gc_object* o) noexcept
    {
        int cnt = o->root_ref_cnt.load(std::memory_order_acquire);
        while (cnt != 0) {
            if (o->root_ref_cnt.compare_exchange_weak(cnt, cnt + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
            {
                return;
            }
            // compare_exchange_weak updated cnt on failure – retry automatically.
        }
        // cnt == 0: GC may be examining this object; hold the lock.
        std::scoped_lock lock{ gc_mutex };
        o->root_ref_cnt.fetch_add(1, std::memory_order_relaxed);
    }

    // Decrement root_ref_cnt.  Never needs the lock.
    void gc_base_ptr::dec_root(gc_object* o) noexcept
    {
        o->root_ref_cnt.fetch_sub(1, std::memory_order_relaxed);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // gc_base_ptr – constructor
    // ─────────────────────────────────────────────────────────────────────────────

    gc_base_ptr::gc_base_ptr(gc_object* o)
    {
        // Decide pointer type based on whether `this` falls inside the object
        // currently being constructed on this thread.
        type = (current &&
            current->start() <= static_cast<void*>(this) &&
            static_cast<void*>(this) < current->end())
            ? PtrType::GC_HEAP
            : PtrType::ROOT;

        if (type == PtrType::GC_HEAP) {
            if (o) {
                std::scoped_lock lock{ gc_mutex };
                object.store(o, std::memory_order_relaxed);
                next.store(current->first.load(std::memory_order_relaxed),std::memory_order_relaxed);
                current->first.store(this, std::memory_order_relaxed);
            }
            else {
                object.store(nullptr, std::memory_order_relaxed);
                next.store(current->first.load(std::memory_order_relaxed),std::memory_order_relaxed);
                // Release fence ensures next is visible before first is updated.
                atomic_thread_fence(std::memory_order_release);
                current->first.store(this, std::memory_order_relaxed);
            }
        }
        else {
            // ROOT pointer (lives on the stack or in static storage).
            object.store(o, std::memory_order_relaxed);
            next.store(nullptr, std::memory_order_relaxed);
            if (o) inc_root(o);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // gc_base_ptr – move constructor
    // ─────────────────────────────────────────────────────────────────────────────

    gc_base_ptr::gc_base_ptr(gc_base_ptr&& o)
    {
        type = (current &&
            current->start() <= static_cast<void*>(this) &&
            static_cast<void*>(this) < current->end())
            ? PtrType::GC_HEAP
            : PtrType::ROOT;

        gc_object* o2 = o.object.load(std::memory_order_relaxed);

        if (type == PtrType::GC_HEAP) {
            if (o2) {
                std::scoped_lock lock{ gc_mutex };
                object.store(o2, std::memory_order_relaxed);
                next.store(current->first.load(std::memory_order_relaxed),std::memory_order_relaxed);
                current->first.store(this, std::memory_order_relaxed);
            }
            else {
                object.store(nullptr, std::memory_order_relaxed);
                next.store(current->first.load(std::memory_order_relaxed),std::memory_order_relaxed);
                atomic_thread_fence(std::memory_order_release);
                current->first.store(this, std::memory_order_relaxed);
            }
        }
        else {
            object.store(o2, std::memory_order_relaxed);
            next.store(nullptr, std::memory_order_relaxed);
            if (o2) {
                if (o.type == PtrType::ROOT) {
                    // Steal the reference: no net change to root_ref_cnt.
                    o.object.store(nullptr, std::memory_order_relaxed);
                }
                else {
                    inc_root(o2);
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // gc_base_ptr – assignment operators
    // ─────────────────────────────────────────────────────────────────────────────

    void gc_base_ptr::operator=(nullptr_t)
    {
        if (type == PtrType::GC_HEAP) {
            object.store(nullptr, std::memory_order_relaxed);
        }
        else {
            gc_object* o = object.exchange(nullptr, std::memory_order_relaxed);
            if (o) dec_root(o);
        }
    }

    void gc_base_ptr::operator=(const gc_base_ptr& o)
    {
        gc_object* o1 = object.load(std::memory_order_relaxed);
        gc_object* o2 = o.object.load(std::memory_order_relaxed);
        if (o1 == o2) return;

        if (type == PtrType::GC_HEAP) {
            if (o2) {
                std::scoped_lock lock{ gc_mutex };
                object.store(o2, std::memory_order_relaxed);
            }
            else {
                object.store(nullptr, std::memory_order_relaxed);
            }
        }
        else {
            if (o1) {
                dec_root(o1);
            }
            object.store(o2, std::memory_order_relaxed);
            if (o2) {
                inc_root(o2);
            }
        }
    }

    void gc_base_ptr::operator=(gc_base_ptr&& o)
    {
        gc_object* o1 = object.load(std::memory_order_relaxed);
        gc_object* o2 = o.object.load(std::memory_order_relaxed);
        if (o1 == o2) return;

        if (type == PtrType::GC_HEAP) {
            if (o2) {
                std::scoped_lock lock{ gc_mutex };
                object.store(o2, std::memory_order_relaxed);
            }
            else {
                object.store(nullptr, std::memory_order_relaxed);
            }
        }
        else {
            if (o.type == PtrType::ROOT) {
                // Swap: reuse both reference counts, no net change.
                object.store(o2, std::memory_order_relaxed);
                o.object.store(o1, std::memory_order_relaxed);
            }
            else {
                if (o1) {
                    dec_root(o1);
                }
                object.store(o2, std::memory_order_relaxed);
                if (o2) {
                    inc_root(o2);
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // gc_base_ptr – reset / destructor
    // ─────────────────────────────────────────────────────────────────────────────

    void gc_base_ptr::reset()
    {
        if (type == PtrType::GC_HEAP) {
            object.store(nullptr, std::memory_order_relaxed);
        }
        else {
            gc_object* o = object.exchange(nullptr, std::memory_order_relaxed);
            if (o) {
                dec_root(o);
            }
        }
    }

    gc_base_ptr::~gc_base_ptr()
    {
        if (type == PtrType::ROOT) {
            gc_object* o = object.load(std::memory_order_relaxed);
            if (o) {
                dec_root(o);
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // gc_collect
    // ─────────────────────────────────────────────────────────────────────────────

    void gc_collect()
    {
        // ── Phases 1-3 run while the mutex is held ────────────────────────────────
        std::vector<gc_object*> garbage;

        {
            std::unique_lock lock{ gc_mutex };

            if (all_objects.empty()) {
                return;
            }
            std::vector<gc_object*> pending;
            pending.reserve(all_objects.size() / 4 + 1);

            // Phase 1: seed pending from root-referenced objects.
            for (gc_object* c : all_objects) {
                if (c->root_ref_cnt.load(std::memory_order_relaxed) != 0) {
                    c->mark = true;
                    atomic_thread_fence(std::memory_order_acquire);
                    for (gc_base_ptr* j = c->first.load(std::memory_order_relaxed);
                        j != nullptr;
                        j = j->next.load(std::memory_order_relaxed))
                    {
                        if (gc_object* o = j->object.load(std::memory_order_relaxed); o) {

                            pending.push_back(o);
                        }
                    }
                }
                else {
                    c->mark = false;
                }
            }

            // Phase 2: transitively mark all reachable objects.
            while (!pending.empty()) {
                gc_object* c = pending.back();
                pending.pop_back();
                if (c->mark) {
                    continue;
                }
                c->mark = true;
                atomic_thread_fence(std::memory_order_acquire);
                for (gc_base_ptr* j = c->first.load(std::memory_order_relaxed);
                    j != nullptr;
                    j = j->next.load(std::memory_order_relaxed))
                {
                    if (gc_object* o = j->object.load(std::memory_order_relaxed);
                        o && !o->mark) {

                        pending.push_back(o); // look here once
                    }
                }
            }

            // Phase 3: partition – live objects first, garbage last.
            auto garbage_begin = std::stable_partition(
                all_objects.begin(), all_objects.end(),
                [](const gc_object* o) { return o->mark; });

            garbage.assign(garbage_begin, all_objects.end());
            all_objects.erase(garbage_begin, all_objects.end());

            // Recalibrate the automatic-collection counter.
            const long new_count = std::max<long>(static_cast<long>(all_objects.size()) * 2L, 1024L);
            gc_counter.store(new_count, std::memory_order_relaxed);

            // lock is released here, before destructors are invoked.
        }

        // ── Phase 4: run destructors (outside the lock) ───────────────────────────
        // Destructors may allocate new GC objects, which would call gc_collect()
        // and try to acquire the mutex.  Releasing it first prevents deadlock.
        for (gc_object* o : garbage) {

            o->~gc_object();
        }

        // ── Phase 5: free memory (re-acquire lock for cache-friendliness) ─────────
        // Benchmarking (from original) shows that a lock here speeds up
        // ::operator delete(), probably due to reduced contention on the allocator.
        {
            std::scoped_lock lock{ gc_mutex };
            for (gc_object* o : garbage)
                ::operator delete(o);
        }
    }

} // namespace gc