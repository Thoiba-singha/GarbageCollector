#pragma once

/**
 * @file gc.hpp
 * @brief Conservative mark-and-sweep garbage collector for C++20.
 *
 
 */

#include <atomic>
#include <concepts>
#include <cstddef>
#include <mutex>
#include <span>
#include <type_traits>
#include <vector>

namespace GC {

    class gc_base_ptr;
    class gc_object;

    // ─────────────────────────────────────────────────────────────────────────────
    // Concepts
    // ─────────────────────────────────────────────────────────────────────────────

    /// Any type that may be heap-allocated through gc::New<T>().
    template <typename T>
    concept GcManaged = std::is_object_v<T>;

    /// Types for which pointer arithmetic on Ptr<T> is meaningful.
    template <typename T>
    concept GcArithmetic = std::is_object_v<T>;

    // ─────────────────────────────────────────────────────────────────────────────
    // Global state
    // ─────────────────────────────────────────────────────────────────────────────

    extern std::mutex                gc_mutex;
    extern thread_local gc_object* current;          ///< object under construction (per thread)
    extern std::vector<gc_object*>   all_objects;       ///< every live gc_object
    extern std::atomic<long>         gc_counter;        ///< countdown to next automatic collection

    /**
     * @brief Automatic trigger a garbage-collection.
     *
     * reaches zero, and once more at program exit.
     */
    void gc_collect();

    // ─────────────────────────────────────────────────────────────────────────────
    // gc_object
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * @brief GC control block.
     *
     * Memory layout: [gc_object][C++ object(s)...]
     *
     * destructor callback receives a std::span<std::byte> that covers
     * exactly the managed objects, making the size implicit and avoiding manual
     * pointer casting at each call site.
     */

    class gc_object {
        template <GcManaged T> friend class New;
        friend class gc_base_ptr;
        friend void gc_collect();

        gc_object(const gc_object&) = delete;
        gc_object& operator=(const gc_object&) = delete;

    protected:
        void* end_;   ///< one-past-end of the managed region

        [[nodiscard]] void* start() noexcept { return static_cast<void*>(this + 1); }
        [[nodiscard]] void* end()   noexcept { return end_; }

        /// Destructor callback – receives [start, end) of the managed region.
        void (*destructor)(std::byte* s, std::byte* e) noexcept;

        std::atomic<int>          root_ref_cnt{ 0 };
        std::atomic<gc_base_ptr*> first{ nullptr };
        bool                      mark{ false };

    public:
        gc_object() noexcept;
        gc_object(void* e, void (*d)(std::byte*, std::byte*) noexcept) noexcept;
        ~gc_object();
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // gc_base_ptr
    // ─────────────────────────────────────────────────────────────────────────────

    class gc_base_ptr {
        friend void gc_collect();
        static void gc_collect(gc_object* o);

    protected:
        enum class PtrType : std::uint8_t { ROOT, GC_HEAP };

        PtrType                   type;
        std::atomic<gc_base_ptr*> next{ nullptr };
        std::atomic<gc_object*>   object{ nullptr };

        /// Increment root_ref_cnt, acquiring the mutex only when necessary.
        static void inc_root(gc_object* o) noexcept;
        /// Decrement root_ref_cnt (never needs the mutex).
        static void dec_root(gc_object* o) noexcept;

    public:
        explicit gc_base_ptr(gc_object* c = nullptr);
        gc_base_ptr(const gc_base_ptr& o) : gc_base_ptr(o.object.load(std::memory_order_relaxed)) {}
        gc_base_ptr(gc_base_ptr&& o);

        void operator=(std::nullptr_t);
        void operator=(const gc_base_ptr& o);
        void operator=(gc_base_ptr&& o);
        void reset();

        ~gc_base_ptr();
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // Ptr<T>
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * @brief Custom Smart pointer whose liveness is tracked by the garbage collector.
     *
     * A Ptr<> that lives on the stack (or in a global) is a *root*; one that
     * lives inside a gc_object is a *heap pointer*.  The distinction is made
     * automatically in the gc_base_ptr constructor by comparing `this` with the
     * currently-constructing object's address range.
     */
    template <GcManaged T>
    class Ptr : public gc_base_ptr {
        template <GcManaged U> friend class Ptr;

    protected:
        T* ptr{ nullptr };

    public:
        // ── Constructors ─────────────────────────────────────────────────────────

        Ptr() noexcept : gc_base_ptr() {}
        Ptr(std::nullptr_t) noexcept : gc_base_ptr() {}

        Ptr(const Ptr& o) : gc_base_ptr(o) { ptr = o.ptr; }
        Ptr(Ptr&& o) : gc_base_ptr(std::move(o)) { ptr = o.ptr; }

        /// Aliasing constructor: tracks @p o for GC purposes, but exposes @p p.
        Ptr(const gc_base_ptr& o, T* p) : gc_base_ptr(o) { ptr = p; }

        /// Implicit up-cast / cross-cast constructor (requires convertibility).
        template <GcManaged U>
            requires std::convertible_to<U*, T*>
        Ptr(const Ptr<U>& o) : gc_base_ptr(o) { ptr = o.ptr; }

        // ── Assignment ───────────────────────────────────────────────────────────

        Ptr& operator=(const Ptr& o) {
            gc_base_ptr::operator=(o);
            ptr = o.ptr;
            return *this;
        }

        Ptr& operator=(Ptr&& o) {
            gc_base_ptr::operator=(std::move(o));
            ptr = o.ptr;
            return *this;
        }

        Ptr& operator=(std::nullptr_t) {
            gc_base_ptr::operator=(nullptr);
            ptr = nullptr;
            return *this;
        }

        // ── Observers ────────────────────────────────────────────────────────────

        [[nodiscard]] T* get()          const noexcept { return ptr; }
        [[nodiscard]] explicit operator bool() const noexcept { return ptr != nullptr; }
        T& operator*()  const noexcept { return *ptr; }
        T* operator->() const noexcept { return ptr; }
        T& operator[](std::ptrdiff_t idx) const noexcept { return ptr[idx]; }

        void reset() { 
            ptr = nullptr; gc_base_ptr::reset();
        }

        // ── Arithmetic (only meaningful for array pointers) ───────────────────────

        Ptr& operator++()    noexcept { ++ptr; return *this; }
        Ptr& operator--()    noexcept { --ptr; return *this; }
        Ptr  operator++(int) noexcept { Ptr r(*this); ptr++; return r; }
        Ptr  operator--(int) noexcept { Ptr r(*this); ptr--; return r; }

        template <std::integral U> Ptr& operator+=(U z) noexcept { ptr += z; return *this; }
        template <std::integral U> Ptr& operator-=(U z) noexcept { ptr -= z; return *this; }
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // New<T>  – single object
    // ─────────────────────────────────────────────────────────────────────────────

    template <GcManaged T>
    class New : public Ptr<T> {
    public:
        template <typename... Args>
            requires std::constructible_from<T, Args...>
        explicit New(Args&&... args)
        {
            auto* mem = static_cast<char*>(::operator new(sizeof(gc_object) + sizeof(T)));

            gc_object* new_obj = nullptr;
            {
                std::scoped_lock lock{ gc_mutex };

                if (gc_counter.fetch_sub(1, std::memory_order_relaxed) <= 0) {
                    // Release the lock before collecting to avoid deadlock.
                    // Unlock manually here since scoped_lock doesn't support that.
                }
                // Re-check after potential collection below
            }

            // Trigger collection outside the lock if the counter expired.
            if (gc_counter.load(std::memory_order_relaxed) < 0) {
                gc_collect();
            }

            {
                std::scoped_lock lock{ gc_mutex };
                new_obj = new (mem) gc_object(
                    mem + sizeof(gc_object) + sizeof(T),
                    [](std::byte* s, std::byte* /*e*/) noexcept {
                        std::destroy_at(reinterpret_cast<T*>(s));
                    });

                gc_base_ptr::object.store(new_obj, std::memory_order_relaxed);
                if (gc_base_ptr::type == gc_base_ptr::PtrType::ROOT)
                    new_obj->root_ref_cnt.store(1, std::memory_order_relaxed);

                all_objects.push_back(new_obj);
            }

            gc_object* parent = current;
            current = new_obj;
            Ptr<T>::ptr = static_cast<T*>(new_obj->start());

            try {
                std::construct_at(static_cast<T*>(new_obj->start()), std::forward<Args>(args)...);
            }
            catch (...) {
                if (gc_base_ptr::type == gc_base_ptr::PtrType::ROOT)
                    new_obj->root_ref_cnt.fetch_sub(1, std::memory_order_relaxed);
                gc_base_ptr::object.store(nullptr, std::memory_order_relaxed);
                Ptr<T>::ptr = nullptr;
                current = parent;
                throw;
            }

            current = parent;
        }
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // New<T[]>  – array of objects
    // ─────────────────────────────────────────────────────────────────────────────

    template <GcManaged T>
    class New<T[]> : public Ptr<T> {
    public:
        explicit New(std::size_t size)
            requires std::default_initializable<T>
        {
            auto* mem = static_cast<char*>(::operator new(sizeof(gc_object) + size * sizeof(T)));

            gc_object* new_obj = nullptr;

            if (gc_counter.fetch_sub(1, std::memory_order_relaxed) <= 0)
                gc_collect();

            {
                std::scoped_lock lock{ gc_mutex };
                new_obj = new (mem) gc_object(
                    mem + sizeof(gc_object) + size * sizeof(T),
                    [](std::byte* s, std::byte* e) noexcept {
                        // Destroy in reverse order (matches construction order).
                        auto* end = reinterpret_cast<T*>(e);
                        auto* begin = reinterpret_cast<T*>(s);
                        std::destroy(std::make_reverse_iterator(end),
                            std::make_reverse_iterator(begin));
                    });

                gc_base_ptr::object.store(new_obj, std::memory_order_relaxed);
                if (gc_base_ptr::type == gc_base_ptr::PtrType::ROOT)
                    new_obj->root_ref_cnt.store(1, std::memory_order_relaxed);

                all_objects.push_back(new_obj);
            }

            gc_object* parent = current;
            current = new_obj;

            T* begin = static_cast<T*>(new_obj->start());
            T* end = static_cast<T*>(new_obj->end());
            Ptr<T>::ptr = begin;

            T* constructed_end = begin;
            try {
                for (T* i = begin; i != end; ++i, ++constructed_end)
                    std::construct_at(i);
            }
            catch (...) {
                std::destroy(begin, constructed_end);
                if (gc_base_ptr::type == gc_base_ptr::PtrType::ROOT)
                    new_obj->root_ref_cnt.fetch_sub(1, std::memory_order_relaxed);
                gc_base_ptr::object.store(nullptr, std::memory_order_relaxed);
                Ptr<T>::ptr = nullptr;
                current = parent;
                throw;
            }

            current = parent;
        }
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // Comparison operators  (Three-way comparison via spaceship where possible)
    // ─────────────────────────────────────────────────────────────────────────────

    template <GcManaged T, GcManaged U>
    [[nodiscard]] bool operator==(const Ptr<T>& a, const Ptr<U>& b) noexcept
    {
        return a.get() == b.get();
    }

    template <GcManaged T>
    [[nodiscard]] bool operator==(const Ptr<T>& a, std::nullptr_t) noexcept
    {
        return a.get() == nullptr;
    }

    template <GcManaged T, GcManaged U>
    [[nodiscard]] auto operator<=>(const Ptr<T>& a, const Ptr<U>& b) noexcept
    {
        return a.get() <=> b.get();
    }

    template <GcManaged T>
    [[nodiscard]] auto operator<=>(const Ptr<T>& a, std::nullptr_t) noexcept
    {
        return a.get() <=> static_cast<T*>(nullptr);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Arithmetic operators
    // ─────────────────────────────────────────────────────────────────────────────

    template <GcManaged T, std::integral U>
    [[nodiscard]] Ptr<T> operator+(const Ptr<T>& a, U b) noexcept
    {
        return Ptr<T>(a, a.get() + b);
    }

    template <GcManaged T, std::integral U>
    [[nodiscard]] Ptr<T> operator+(U a, const Ptr<T>& b) noexcept
    {
        return Ptr<T>(b, b.get() + a);
    }

    template <GcManaged T, std::integral U>
    [[nodiscard]] Ptr<T> operator-(const Ptr<T>& a, U b) noexcept
    {
        return Ptr<T>(a, a.get() - b);
    }

    template <GcManaged T>
    [[nodiscard]] std::ptrdiff_t operator-(const Ptr<T>& a, const Ptr<T>& b) noexcept
    {
        return a.get() - b.get();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Cast helpers
    // ─────────────────────────────────────────────────────────────────────────────

    template <GcManaged T, GcManaged U>
    [[nodiscard]] Ptr<T> static_pointer_cast(const Ptr<U>& p) noexcept
    {
        return Ptr<T>(p, static_cast<T*>(p.get()));
    }

    template <GcManaged T, GcManaged U>
    [[nodiscard]] Ptr<T> dynamic_pointer_cast(const Ptr<U>& p) noexcept
    {
        return Ptr<T>(p, dynamic_cast<T*>(p.get()));
    }

    template <GcManaged T, GcManaged U>
    [[nodiscard]] Ptr<T> const_pointer_cast(const Ptr<U>& p) noexcept
    {
        return Ptr<T>(p, const_cast<T*>(p.get()));
    }

    template <GcManaged T, GcManaged U>
    [[nodiscard]] Ptr<T> reinterpret_pointer_cast(const Ptr<U>& p) noexcept
    {
        return Ptr<T>(p, reinterpret_cast<T*>(p.get()));
    }

} // namespace gc