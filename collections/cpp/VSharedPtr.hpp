#pragma once

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>
#include <cassert>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <string>
#include <concepts>
#include <stdexcept>
#include <source_location>
#include <compare>
#include <cstdint>

namespace ptr {

    // =============================================================================
    // ptr::meta
    // =============================================================================
    namespace meta {

        enum class ThreadMode : bool {
            True = true,   ///< Thread-safe (atomics + auto-locking proxy)
            False = false   ///< Non-thread-safe (plain types, faster)
        };

        // Single source of truth for safety checks — no mixed #if / constexpr
#ifndef VSHAREDPTR_SAFETY_CHECKS
#   ifdef NDEBUG
        inline constexpr bool safety_checks_enabled = false;
#   else
        inline constexpr bool safety_checks_enabled = true;
#   endif
#else
        inline constexpr bool safety_checks_enabled = (VSHAREDPTR_SAFETY_CHECKS != 0);
#endif

        namespace magic {
            inline constexpr uint64_t alive = 0xDEADBEEFCAFEBABEULL;
            inline constexpr uint64_t destroyed = 0xDEADDEADDEADDEADULL;
        }

        template<ThreadMode TM>
        struct MemoryOrder {
            static constexpr auto acquire = (TM == ThreadMode::True)
                ? std::memory_order_acquire : std::memory_order_relaxed;
            static constexpr auto release = (TM == ThreadMode::True)
                ? std::memory_order_release : std::memory_order_relaxed;
            static constexpr auto acq_rel = (TM == ThreadMode::True)
                ? std::memory_order_acq_rel : std::memory_order_relaxed;
            static constexpr auto seq_cst = (TM == ThreadMode::True)
                ? std::memory_order_seq_cst : std::memory_order_relaxed;
            static constexpr auto relaxed = std::memory_order_relaxed;
        };

    } // namespace meta

    // =============================================================================
    // ptr::traits
    // =============================================================================
    namespace traits {

        template<typename T>
        concept NotArray = !std::is_array_v<T>;

        template<typename T>
        concept IsUnboundedArray = std::is_array_v<T> && (std::extent_v<T> == 0);

        template<typename From, typename To>
        concept ConvertiblePtr = std::is_convertible_v<From*, To*>;

        template<meta::ThreadMode TM, typename T>
        using RefCountType = std::conditional_t<TM == meta::ThreadMode::True,
            std::atomic<T>, T>;

        template<meta::ThreadMode TM, typename T>
        using PtrType = std::conditional_t<TM == meta::ThreadMode::True,
            std::atomic<T*>, T*>;

    } // namespace traits

    // =============================================================================
    // ptr::exception
    // =============================================================================
    namespace exception {

        class MemorySafety : public std::runtime_error {
            std::source_location location_;
        public:
            explicit MemorySafety(
                const std::string& message,
                std::source_location loc = std::source_location::current())
                : std::runtime_error(message), location_(loc) {}

            [[nodiscard]] const std::source_location& where() const noexcept { return location_; }

            [[nodiscard]] std::string diagnostic() const {
                return std::string(what())
                    + "\n  at " + location_.file_name()
                    + ":" + std::to_string(location_.line())
                    + ":" + std::to_string(location_.column())
                    + " in " + location_.function_name();
            }
        };

        [[noreturn]] inline void throw_or_abort(const char* msg,
            std::source_location loc = std::source_location::current())
        {
            if constexpr (meta::safety_checks_enabled) {
                throw MemorySafety(msg, loc);
            }
            else {
                assert(false && "VSharedPtr safety violation");
                std::abort();
            }
        }

    } // namespace exception

    // =============================================================================
    // ptr::detail
    // =============================================================================
    namespace detail {

        // =========================================================================
        // LockedProxy — RAII exclusive-lock wrapper returned by operator->
        // =========================================================================
        template<typename T>
        class LockedProxy {
            T* ptr_;
            std::unique_lock<std::shared_mutex>   lock_;
        public:
            LockedProxy(T* ptr, std::shared_mutex& mtx)
                : ptr_(ptr), lock_(mtx)
            {
                if (!ptr_) exception::throw_or_abort("LockedProxy: null pointer");
            }

            LockedProxy(const LockedProxy&) = delete;
            LockedProxy& operator=(const LockedProxy&) = delete;

            LockedProxy(LockedProxy&& o) noexcept
                : ptr_(o.ptr_), lock_(std::move(o.lock_)) {
                o.ptr_ = nullptr;
            }

            LockedProxy& operator=(LockedProxy&& o) noexcept {
                if (this != &o) { ptr_ = o.ptr_; lock_ = std::move(o.lock_); o.ptr_ = nullptr; }
                return *this;
            }

            [[nodiscard]] T* operator->() const noexcept { assert(ptr_); return ptr_; }
            [[nodiscard]] T& operator*()  const noexcept { assert(ptr_); return *ptr_; }
            [[nodiscard]] T* get()        const noexcept { return ptr_; }
            [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }
        };

        // =========================================================================
        // ControlBlock — reference counting + managed object lifetime
        // =========================================================================
        template<typename T, meta::ThreadMode TM>
        class ControlBlock {
            using MO = meta::MemoryOrder<TM>;

            // Ref counts — separate to avoid false sharing
            alignas(64) traits::RefCountType<TM, size_t> gc_strong_count_;
            alignas(64) traits::RefCountType<TM, size_t> gc_weak_count_;
            traits::PtrType<TM, T>                       ptr_;
            traits::RefCountType<TM, bool>               object_destroyed_;
            const bool                                   is_array_;

            // Protects object access; upgrade to exclusive for destruction
            mutable std::shared_mutex object_mutex_;

            // Corruption detection — always compiled in, gated at runtime
            uint64_t                  magic_header_{ meta::magic::alive };
            uint64_t                  magic_footer_{ meta::magic::alive };
            std::atomic<bool>         control_block_destroyed_{ false };

            // ---- helpers --------------------------------------------------------

            [[nodiscard]] size_t load_count(const traits::RefCountType<TM, size_t>& c) const noexcept {
                if constexpr (TM == meta::ThreadMode::True)
                    return c.load(MO::acquire);
                else
                    return c;
            }

            [[nodiscard]] T* load_ptr() const noexcept {
                if constexpr (TM == meta::ThreadMode::True)
                    return ptr_.load(MO::acquire);
                else
                    return ptr_;
            }

            T* exchange_ptr_impl(T* p) noexcept {
                if constexpr (TM == meta::ThreadMode::True)
                    return ptr_.exchange(p, MO::acq_rel);
                else { T* old = ptr_; ptr_ = p; return old; }
            }

            [[nodiscard]] bool load_destroyed() const noexcept {
                if constexpr (TM == meta::ThreadMode::True)
                    return object_destroyed_.load(MO::acquire);
                else
                    return object_destroyed_;
            }

            void verify_integrity() const {
                if (!meta::safety_checks_enabled) return;
                if (magic_header_ != meta::magic::alive ||
                    magic_footer_ != meta::magic::alive)
                    throw exception::MemorySafety("ControlBlock: corruption detected!");
                if (control_block_destroyed_.load(std::memory_order_acquire))
                    throw exception::MemorySafety("ControlBlock: already destroyed!");
            }

            void destroy_object() noexcept {
                // Fast check before acquiring anything
                if (load_destroyed()) return;

                if constexpr (TM == meta::ThreadMode::True) {
                    bool expected = false;
                    if (!object_destroyed_.compare_exchange_strong(
                        expected, true, MO::acq_rel, MO::acquire))
                        return; // Another thread won the race
                }
                else {
                    if (object_destroyed_) return;
                    object_destroyed_ = true;
                }

                // Upgrade: acquire exclusive lock before touching the object.
                // Readers hold shared_lock; we wait for them to drain.
                {
                    std::unique_lock lock(object_mutex_);
                    T* p = exchange_ptr_impl(nullptr);
                    if (p) {
                        if (is_array_) delete[] p;
                        else           delete p;
                    }
                }
            }

        public:
            explicit ControlBlock(T* p, bool is_array = false) noexcept
                : gc_strong_count_(1)
                , gc_weak_count_(0)
                , ptr_(p)
                , object_destroyed_(false)
                , is_array_(is_array)
            {
            }

            ~ControlBlock() {
                control_block_destroyed_.store(true, std::memory_order_release);
                magic_header_ = meta::magic::destroyed;
                magic_footer_ = meta::magic::destroyed;
            }

            ControlBlock(const ControlBlock&) = delete;
            ControlBlock& operator=(const ControlBlock&) = delete;
            ControlBlock(ControlBlock&&) = delete;
            ControlBlock& operator=(ControlBlock&&) = delete;

            void add_strong() {
                verify_integrity();
                if constexpr (TM == meta::ThreadMode::True)
                    gc_strong_count_.fetch_add(1, MO::relaxed);
                else
                    ++gc_strong_count_;
            }

            void add_weak() {
                verify_integrity();
                if constexpr (TM == meta::ThreadMode::True)
                    gc_weak_count_.fetch_add(1, MO::relaxed);
                else
                    ++gc_weak_count_;
            }

            [[nodiscard]] bool try_add_strong() noexcept {
                if constexpr (TM == meta::ThreadMode::True) {
                    size_t count = gc_strong_count_.load(MO::acquire);
                    while (count > 0) {
                        if (gc_strong_count_.compare_exchange_weak(
                            count, count + 1, MO::acq_rel, MO::acquire))
                            return true;
                        // count updated by CAS failure — loop
                    }
                    return false;
                }
                else {
                    if (gc_strong_count_ > 0) { ++gc_strong_count_; return true; }
                    return false;
                }
            }

            // FIX: read weak_count BEFORE the potential `delete this` path
            void release_strong() noexcept {
                size_t old_count;
                if constexpr (TM == meta::ThreadMode::True)
                    old_count = gc_strong_count_.fetch_sub(1, MO::release);
                else
                    old_count = gc_strong_count_--;

                if (old_count == 0) return; // underflow guard

                if (old_count == 1) {
                    // Synchronize with all prior releases
                    if constexpr (TM == meta::ThreadMode::True)
                        std::atomic_thread_fence(MO::acquire);

                    destroy_object();

                    // Read weak count NOW, before any `delete this`
                    size_t weak = load_count(gc_weak_count_);
                    if (weak == 0) {
                        if constexpr (TM == meta::ThreadMode::True)
                            std::atomic_thread_fence(MO::acquire);
                        delete this; // safe — we are the last reference
                    }
                }
            }

            // FIX: read strong_count BEFORE the potential `delete this` path
            void release_weak() noexcept {
                size_t old_count;
                if constexpr (TM == meta::ThreadMode::True)
                    old_count = gc_weak_count_.fetch_sub(1, MO::release);
                else
                    old_count = gc_weak_count_--;

                if (old_count == 0) return; // underflow guard

                if (old_count == 1) {
                    if constexpr (TM == meta::ThreadMode::True)
                        std::atomic_thread_fence(MO::acquire);

                    // Read strong count NOW, before any `delete this`
                    size_t strong = load_count(gc_strong_count_);
                    if (strong == 0) {
                        if constexpr (TM == meta::ThreadMode::True)
                            std::atomic_thread_fence(MO::acquire);
                        delete this;
                    }
                }
            }

            [[nodiscard]] T* get_ptr()      const noexcept { return load_ptr(); }
            [[nodiscard]] bool          is_alive()      const noexcept { return load_count(gc_strong_count_) > 0; }
            [[nodiscard]] size_t        strong_count()  const noexcept { return load_count(gc_strong_count_); }
            [[nodiscard]] size_t        weak_count()    const noexcept { return load_count(gc_weak_count_); }
            [[nodiscard]] bool          is_array()      const noexcept { return is_array_; }
            [[nodiscard]] std::shared_mutex& get_mutex() const noexcept { return object_mutex_; }
        };

    } // namespace detail

    // =============================================================================
    // VSharedPtr — main smart pointer
    // =============================================================================
    template<typename T, meta::ThreadMode TM = meta::ThreadMode::True>
    class VSharedPtr {
    public:
        using element_type = std::remove_extent_t<T>;
        using pointer = element_type*;
        using reference = element_type&;

    private:
        using Elem = element_type;
        using CB = detail::ControlBlock<Elem, TM>;
        using MO = meta::MemoryOrder<TM>;

        traits::PtrType<TM, CB>      ctrl_;
        traits::RefCountType<TM, bool> is_weak_;

        explicit VSharedPtr(CB* ctrl, bool is_weak) noexcept
            : ctrl_(ctrl), is_weak_(is_weak) {
        }

        template<typename U, meta::ThreadMode TM2> friend class VSharedPtr;

        // ---- atomic/plain load/store/exchange helpers --------------------------
        [[nodiscard]] CB* load_ctrl() const noexcept {
            if constexpr (TM == meta::ThreadMode::True) return ctrl_.load(MO::acquire);
            else return ctrl_;
        }
        void store_ctrl(CB* c) noexcept {
            if constexpr (TM == meta::ThreadMode::True) ctrl_.store(c, MO::release);
            else ctrl_ = c;
        }
        CB* exchange_ctrl(CB* c) noexcept {
            if constexpr (TM == meta::ThreadMode::True) return ctrl_.exchange(c, MO::acq_rel);
            else { CB* old = ctrl_; ctrl_ = c; return old; }
        }
        [[nodiscard]] bool load_weak() const noexcept {
            if constexpr (TM == meta::ThreadMode::True) return is_weak_.load(MO::acquire);
            else return is_weak_;
        }
        void store_weak(bool w) noexcept {
            if constexpr (TM == meta::ThreadMode::True) is_weak_.store(w, MO::release);
            else is_weak_ = w;
        }
        bool exchange_weak(bool w) noexcept {
            if constexpr (TM == meta::ThreadMode::True) return is_weak_.exchange(w, MO::acq_rel);
            else { bool old = is_weak_; is_weak_ = w; return old; }
        }

        void release() noexcept {
            if (CB* c = load_ctrl()) {
                if (load_weak()) c->release_weak();
                else             c->release_strong();
            }
        }

    public:
        // ---- constructors -------------------------------------------------------
        constexpr VSharedPtr() noexcept : ctrl_(nullptr), is_weak_(false) {}
        constexpr VSharedPtr(std::nullptr_t) noexcept : ctrl_(nullptr), is_weak_(false) {}

        explicit VSharedPtr(Elem* ptr) : ctrl_(nullptr), is_weak_(false) {
            if (!ptr) return;
            constexpr bool arr = std::is_array_v<T>;
            CB* cb = nullptr;
            try {
                cb = new CB(ptr, arr);
            }
            catch (...) {
                if constexpr (arr) delete[] ptr;
                else               delete ptr;
                throw;
            }
            store_ctrl(cb);
        }

        VSharedPtr(const VSharedPtr& o) noexcept : ctrl_(nullptr), is_weak_(false) {
            CB* c = o.load_ctrl();
            bool w = o.load_weak();
            if (c) { if (w) c->add_weak(); else c->add_strong(); }
            store_ctrl(c);
            store_weak(w);
        }

        VSharedPtr(VSharedPtr&& o) noexcept
            : ctrl_(o.exchange_ctrl(nullptr))
            , is_weak_(o.exchange_weak(false))
        {
        }

        // Cross-type copy (same ThreadMode, compatible pointers, no UB reinterpret_cast)
        // We restrict this to T == U or U* -> T* for same Elem type to avoid UB.
        template<typename U, meta::ThreadMode TM2>
            requires traits::ConvertiblePtr<std::remove_extent_t<U>, Elem>
        && (TM == TM2)
            && std::is_same_v<std::remove_extent_t<U>, Elem>
            VSharedPtr(const VSharedPtr<U, TM2>& o) noexcept : ctrl_(nullptr), is_weak_(false) {
            CB* c = reinterpret_cast<CB*>(o.load_ctrl()); // safe: same Elem type
            bool w = o.load_weak();
            if (c) { if (w) c->add_weak(); else c->add_strong(); }
            store_ctrl(c);
            store_weak(w);
        }

        template<typename U, meta::ThreadMode TM2>
            requires traits::ConvertiblePtr<std::remove_extent_t<U>, Elem>
        && (TM == TM2)
            && std::is_same_v<std::remove_extent_t<U>, Elem>
            VSharedPtr(VSharedPtr<U, TM2>&& o) noexcept : ctrl_(nullptr), is_weak_(false) {
            store_ctrl(reinterpret_cast<CB*>(o.exchange_ctrl(nullptr)));
            store_weak(o.exchange_weak(false));
        }

        ~VSharedPtr() { release(); }

        // ---- assignment ---------------------------------------------------------
        VSharedPtr& operator=(const VSharedPtr& o) noexcept {
            if (this != &o) { VSharedPtr tmp(o); swap(tmp); }
            return *this;
        }
        VSharedPtr& operator=(VSharedPtr&& o) noexcept {
            if (this != &o) { release(); store_ctrl(o.exchange_ctrl(nullptr)); store_weak(o.exchange_weak(false)); }
            return *this;
        }
        VSharedPtr& operator=(std::nullptr_t) noexcept { reset(); return *this; }

        // ---- dereference --------------------------------------------------------
        template<typename U = T> requires traits::NotArray<U>
        [[nodiscard]] auto operator->() const {
            CB* c = load_ctrl();
            bool w = load_weak();
            if (w)  exception::throw_or_abort("operator->: cannot dereference weak pointer");
            if (!c) exception::throw_or_abort("operator->: null pointer dereference");
            if constexpr (TM == meta::ThreadMode::True)
                return detail::LockedProxy<Elem>(c->get_ptr(), c->get_mutex());
            else
                return c->get_ptr();
        }

        template<typename U = T> requires traits::NotArray<U>
        [[nodiscard]] Elem& operator*() const {
            CB* c = load_ctrl();
            bool w = load_weak();
            if (w)  exception::throw_or_abort("operator*: cannot dereference weak pointer");
            if (!c) exception::throw_or_abort("operator*: null pointer dereference");
            if constexpr (TM == meta::ThreadMode::True) {
                std::shared_lock lock(c->get_mutex());
                return *c->get_ptr();
            }
            else {
                return *c->get_ptr();
            }
        }

        template<typename U = T> requires traits::IsUnboundedArray<U>
        [[nodiscard]] Elem& operator[](size_t index) const {
            CB* c = load_ctrl();
            bool w = load_weak();
            if (w)  exception::throw_or_abort("operator[]: cannot access weak pointer");
            if (!c) exception::throw_or_abort("operator[]: null pointer access");
            if constexpr (TM == meta::ThreadMode::True) {
                std::shared_lock lock(c->get_mutex());
                return c->get_ptr()[index];
            }
            else {
                return c->get_ptr()[index];
            }
        }

        // ---- access -------------------------------------------------------------
        [[nodiscard]] Elem* get() const noexcept {
            CB* c = load_ctrl();
            if (!c || load_weak()) return nullptr;
            return c->get_ptr();
        }

        template<meta::ThreadMode M = TM>
            requires (M == meta::ThreadMode::True) && traits::NotArray<T>
        [[nodiscard]] detail::LockedProxy<Elem> lock_access() const {
            CB* c = load_ctrl();
            bool w = load_weak();
            if (w)  exception::throw_or_abort("lock_access: called on weak pointer");
            if (!c) exception::throw_or_abort("lock_access: called on null pointer");
            return detail::LockedProxy<Elem>(c->get_ptr(), c->get_mutex());
        }

        // ---- weak pointer support -----------------------------------------------

        /// Create a weak handle pointing at the same object as `strong_ref`
        [[nodiscard]] VSharedPtr make_weak(const VSharedPtr& strong_ref) const {
            CB* c = strong_ref.load_ctrl();
            bool w = strong_ref.load_weak();
            if (!c || w) return VSharedPtr();
            VSharedPtr wp;
            wp.store_ctrl(c);
            wp.store_weak(true);
            c->add_weak();
            return wp;
        }

        /// Promote a weak pointer to a strong one (returns null VSharedPtr on expiry)
        [[nodiscard]] VSharedPtr lock() const {
            if (!load_weak()) return VSharedPtr(*this); // already strong
            CB* c = load_ctrl();
            if (!c) return VSharedPtr();
            if (c->try_add_strong()) return VSharedPtr(c, false);
            return VSharedPtr();
        }

        /// Make this pointer a weak reference to `other` (strong)
        void weak(const VSharedPtr& other) {
            if (this == &other) return;
            release();
            CB* c = other.load_ctrl();
            bool w = other.load_weak();
            if (c && !w) {
                c->add_weak();
                store_ctrl(c);
                store_weak(true);
            }
            else {
                store_ctrl(nullptr);
                store_weak(false);
            }
        }

        // ---- queries ------------------------------------------------------------
        [[nodiscard]] bool expired() const noexcept {
            CB* c = load_ctrl();
            return !c || !c->is_alive();
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return load_weak() ? !expired() : (get() != nullptr);
        }

        [[nodiscard]] size_t ref_count()  const noexcept { CB* c = load_ctrl(); return c ? c->strong_count() : 0; }
        [[nodiscard]] size_t weak_count() const noexcept { CB* c = load_ctrl(); return c ? c->weak_count() : 0; }
        [[nodiscard]] bool   unique()     const noexcept { return ref_count() == 1; }
        [[nodiscard]] bool   is_weak()    const noexcept { return load_weak(); }
        [[nodiscard]] static constexpr meta::ThreadMode thread_mode() noexcept { return TM; }

        // ---- mutation -----------------------------------------------------------
        void reset() noexcept { release(); store_ctrl(nullptr); store_weak(false); }

        void reset(Elem* ptr) { VSharedPtr tmp(ptr); swap(tmp); }

        // FIX: swap is inherently non-atomic for two-field types; document and
        // guard with a note. For truly atomic swap callers must use external sync.
        void swap(VSharedPtr& other) noexcept {
            if constexpr (TM == meta::ThreadMode::True) {
                // Two separate exchanges — not a single atomic op, but consistent
                // for single-threaded swap callers. Callers requiring atomic swap
                // must provide their own external synchronization.
                CB* my_ctrl = exchange_ctrl(other.load_ctrl());
                bool my_weak = exchange_weak(other.load_weak());
                other.store_ctrl(my_ctrl);
                other.store_weak(my_weak);
            }
            else {
                std::swap(ctrl_, other.ctrl_);
                std::swap(is_weak_, other.is_weak_);
            }
        }

        // ---- comparison ---------------------------------------------------------
        [[nodiscard]] bool operator==(const VSharedPtr& o) const noexcept { return get() == o.get(); }
        [[nodiscard]] bool operator!=(const VSharedPtr& o) const noexcept { return !(*this == o); }
        [[nodiscard]] bool operator==(std::nullptr_t)      const noexcept { return get() == nullptr; }
        [[nodiscard]] bool operator!=(std::nullptr_t)      const noexcept { return get() != nullptr; }
        [[nodiscard]] auto operator<=>(const VSharedPtr& o) const noexcept { return get() <=> o.get(); }
    };

    // =============================================================================
    // Factory functions
    // =============================================================================
    template<typename T, meta::ThreadMode TM = meta::ThreadMode::True, typename... Args>
        requires traits::NotArray<T>
    [[nodiscard]] VSharedPtr<T, TM> VMakeShared(Args&&... args) {
        return VSharedPtr<T, TM>(new T(std::forward<Args>(args)...));
    }

    template<typename T, meta::ThreadMode TM = meta::ThreadMode::True>
        requires traits::IsUnboundedArray<T>
    [[nodiscard]] VSharedPtr<T, TM> VMakeShared(size_t count) {
        return VSharedPtr<T, TM>(new std::remove_extent_t<T>[count]());
    }

    // =============================================================================
    // Type aliases
    // =============================================================================
    template<typename T> using VSharedPtrThreadSafe = VSharedPtr<T, meta::ThreadMode::True>;
    template<typename T> using VSharedPtrFast = VSharedPtr<T, meta::ThreadMode::False>;

    // =============================================================================
    // Free swap
    // =============================================================================
    template<typename T, meta::ThreadMode TM>
    void swap(VSharedPtr<T, TM>& a, VSharedPtr<T, TM>& b) noexcept { a.swap(b); }

    // =============================================================================
    // Convenience macro (unchanged for backward compat)
    // =============================================================================
#define WEAK_ASSIGN(ptr, member, value) (ptr)->member.weak(value)

} // namespace ptr


