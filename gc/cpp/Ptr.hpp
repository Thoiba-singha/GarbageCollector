#pragma once
#include <iostream>
#include <unordered_set>
#include <mutex>
#include <functional>
#include <new>
#include <algorithm>
#include <type_traits>
#include <memory>

namespace GC {

    // =============================================================
    // Header
    // =============================================================
    struct Header {
        bool marked = false;
        size_t size = 0;
        std::function<void(const std::function<void(Header*)>&)> trace;
        std::function<void()> destroy;
    };

    // =============================================================
    // Runtime
    // =============================================================
    class Runtime {
    public:
        static Runtime& instance() {
            static Runtime r;
            return r;
        }

        void register_obj(Header* h) {
            std::lock_guard lock(mutex);
            objects.insert(h);
            allocated += h->size;

            if (allocated > hard_limit) {
                pending_full_gc = true;
            }
            else if (++alloc_counter >= alloc_trigger) {

                pending_incremental_gc = true;
            }

            try_run_gc();
        }

        void add_root(Header* h) {
            roots.insert(h);
        }

        void remove_root(Header* h) {
            roots.erase(h);
            pending_full_gc = true;
        }

    private:
        Runtime() = default;

        void try_run_gc() {
            if (gc_running) {
                return;
            }
            if (!pending_full_gc && !pending_incremental_gc) {
                return;
            }
            gc_running = true;
            pending_full_gc ? collect_full() : collect_incremental();
            pending_full_gc = pending_incremental_gc = false;
            gc_running = false;
        }

        void mark() {
            for (auto* h : objects) {

                h->marked = false;
            }
            for (auto* r : roots) {

                dfs(r);
            }
        }

        void dfs(Header* h) {
            if (!h || h->marked) return;
            h->marked = true;
            h->trace([this](Header* c) { dfs(c); });
        }

        void collect_incremental() {
            mark();
            sweep_budgeted();
            alloc_counter = 0;
        }

        void collect_full() {
            mark();
            sweep_all();
            alloc_counter = 0;
        }

        void sweep_all() {
            for (auto it = objects.begin(); it != objects.end();) {
                if (!(*it)->marked) {
                    auto* dead = *it;
                    it = objects.erase(it);
                    allocated -= dead->size;
                    dead->destroy();
                }
                else ++it;
            }
        }

        void sweep_budgeted() {
            size_t freed = 0;
            for (auto it = objects.begin();
                it != objects.end() && freed < sweep_budget;) {

                if (!(*it)->marked) {
                    auto* dead = *it;
                    it = objects.erase(it);
                    freed += dead->size;
                    allocated -= dead->size;
                    dead->destroy();
                }
                else ++it;
            }
        }

        ~Runtime() { collect_full(); }

    private:
        std::unordered_set<Header*> objects;
        std::unordered_set<Header*> roots;
        std::mutex mutex;

        size_t allocated = 0;
        size_t alloc_counter = 0;
        bool pending_full_gc = false;
        bool pending_incremental_gc = false;
        bool gc_running = false;

        static constexpr size_t alloc_trigger = 128;
        static constexpr size_t sweep_budget = 256 * 1024;
        static constexpr size_t hard_limit = 8 * 1024 * 1024;
    };

    // =============================================================
    // Ptr<T>
    // =============================================================
    template<typename T>
    class Ptr {
    public:
        Ptr() = default;
        explicit Ptr(Header* h) : header(h) {
            if (header) Runtime::instance().add_root(header);
        }
        Ptr(const Ptr& o) : Ptr(o.header) {}
        Ptr& operator=(const Ptr& o) {
            if (this != &o) reset(), header = o.header,
                Runtime::instance().add_root(header);
            return *this;
        }
        ~Ptr() { reset(); }

        T* operator->() const { return get(); }
        T* get() const { return reinterpret_cast<T*>(header + 1); }
        explicit operator bool() const { return header != nullptr; }

    private:
        void reset() {
            if (header) {
                Runtime::instance().remove_root(header);
            }
            header = nullptr;
        }
        Header* header = nullptr;
    };

    // =============================================================
    // Ptr<T[]>
    // =============================================================
    template<typename T>
    class Ptr<T[]> {
    public:
        Ptr() = default;
        explicit Ptr(Header* h) : header(h) {
            if (header) Runtime::instance().add_root(header);
        }
        Ptr(const Ptr& o) : Ptr(o.header) {}
        Ptr& operator=(const Ptr& o) {
            if (this != &o) reset(), header = o.header,
                Runtime::instance().add_root(header);
            return *this;
        }
        ~Ptr() { reset(); }

        T& operator[](size_t i) const {
            return reinterpret_cast<T*>(header + 1)[i];
        }

        T* get() const {
            return reinterpret_cast<T*>(header + 1);
        }

        explicit operator bool() const { return header != nullptr; }

    private:
        void reset() {
            if (header) {
                Runtime::instance().remove_root(header);
            }
            header = nullptr;
        }
        Header* header = nullptr;
    };

    // =============================================================
    // New<T>  (non-array)
    // =============================================================
    template<typename T, typename... Args>
        requires (!std::is_array_v<T>)
    Ptr<T> New(Args&&... args) {
        constexpr size_t ALIGN = std::max(alignof(Header), alignof(T));
        const size_t total = sizeof(Header) + sizeof(T);

        void* mem = ::operator new(total, std::align_val_t{ ALIGN });

        auto* h = new (mem) Header{};
        auto* obj = new (static_cast<char*>(mem) + sizeof(Header))
            T(std::forward<Args>(args)...);

        h->size = total;
        h->trace = [](auto) {};
        h->destroy = [obj, mem]() {
            std::destroy_at(obj);
            ::operator delete(mem, std::align_val_t{ ALIGN });
            };

        Runtime::instance().register_obj(h);
        return Ptr<T>(h);
    }

    // =============================================================
    // New<T[]>  (unbounded array)
    // =============================================================
    template<typename T>
        requires std::is_unbounded_array_v<T>
    Ptr<T> New(size_t count) {
        using Elem = std::remove_extent_t<T>;

        constexpr size_t ALIGN =
            std::max(alignof(Header), alignof(Elem));

        const size_t total = sizeof(Header) + sizeof(Elem) * count;

        void* mem = ::operator new(total, std::align_val_t{ ALIGN });

        auto* h = new (mem) Header{};
        auto* arr = reinterpret_cast<Elem*>(
            static_cast<char*>(mem) + sizeof(Header));

        std::uninitialized_value_construct_n(arr, count);

        h->size = total;
        h->trace = [](auto) {};
        h->destroy = [arr, count, mem]() {
            std::destroy_n(arr, count);
            ::operator delete(mem, std::align_val_t{ ALIGN });
            };

        Runtime::instance().register_obj(h);
        return Ptr<T>(h);
    }

} // namespace GC
