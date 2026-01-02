#pragma once

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>
#include <cassert>
#include <thread>
#include <vector>
#include <chrono>
#include <mutex>
#include <functional>
#include <string>

namespace GC {

    template<typename T> class VSharedPtr;

    template<typename T>
    class ControlBlock {
    private:
        std::atomic<size_t> gc_strong_count_;
        std::atomic<size_t> gc_weak_count_;
        std::atomic<T*> ptr_;
        std::atomic<bool> object_destroyed_;
        bool is_array_;

    public:
        explicit ControlBlock(T* p, bool is_array = false) noexcept
            : gc_strong_count_(1), gc_weak_count_(0), ptr_(p), object_destroyed_(false), is_array_(is_array) {
        }

        ControlBlock(const ControlBlock&) = delete;
        ControlBlock& operator=(const ControlBlock&) = delete;

        void add_strong() noexcept {
            gc_strong_count_.fetch_add(1, std::memory_order_relaxed);
        }

        void add_weak() noexcept {
            gc_weak_count_.fetch_add(1, std::memory_order_relaxed);
        }

        bool try_add_strong() noexcept {
            size_t count = gc_strong_count_.load(std::memory_order_acquire);
            while (count > 0) {
                if (gc_strong_count_.compare_exchange_weak(
                    count, count + 1,
                    std::memory_order_acquire,
                    std::memory_order_acquire)) {
                    return true;
                }
            }
            return false;
        }

        void release_strong() noexcept {
            if (gc_strong_count_.fetch_sub(1, std::memory_order_release) == 1) {
                std::atomic_thread_fence(std::memory_order_acquire);
                destroy_object();
                if (gc_weak_count_.load(std::memory_order_acquire) == 0) {
                    std::atomic_thread_fence(std::memory_order_acquire);
                    delete this;
                }
            }
        }

        void release_weak() noexcept {
            if (gc_weak_count_.fetch_sub(1, std::memory_order_release) == 1) {
                std::atomic_thread_fence(std::memory_order_acquire);
                if (gc_strong_count_.load(std::memory_order_acquire) == 0) {
                    std::atomic_thread_fence(std::memory_order_acquire);
                    delete this;
                }
            }
        }

        T* get_ptr() const noexcept {
            return ptr_.load(std::memory_order_acquire);
        }

        bool is_alive() const noexcept {
            return gc_strong_count_.load(std::memory_order_acquire) > 0;
        }

        size_t strong_count() const noexcept {
            return gc_strong_count_.load(std::memory_order_acquire);
        }

        size_t weak_count() const noexcept {
            return gc_weak_count_.load(std::memory_order_acquire);
        }

        bool is_array() const noexcept {
            return is_array_;
        }

    private:
        void destroy_object() noexcept {
            bool expected = false;
            if (object_destroyed_.compare_exchange_strong(
                expected, true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {

                T* p = ptr_.exchange(nullptr, std::memory_order_acq_rel);
                if (p) {
                    if (is_array_) {
                        delete[] p;
                    }
                    else {
                        delete p;
                    }
                }
            }
        }
    };

    // Base template for non-array types
    template<typename T>
    class VSharedPtr {
    private:
        std::atomic<ControlBlock<T>*> ctrl_;
        std::atomic<bool> is_weak_;

        explicit VSharedPtr(ControlBlock<T>* ctrl, bool is_weak) noexcept
            : ctrl_(ctrl), is_weak_(is_weak) {
        }

        template<typename U> friend class VSharedPtr;

    public:
        constexpr VSharedPtr() noexcept : ctrl_(nullptr), is_weak_(false) {}
        constexpr VSharedPtr(std::nullptr_t) noexcept : ctrl_(nullptr), is_weak_(false) {}

        explicit VSharedPtr(T* ptr) : ctrl_(nullptr), is_weak_(false) {
            if (ptr) {
                try {
                    ctrl_.store(new ControlBlock<T>(ptr, false), std::memory_order_release);
                }
                catch (...) {
                    delete ptr;
                    throw;
                }
            }
        }

        VSharedPtr(const VSharedPtr& other) noexcept : ctrl_(nullptr), is_weak_(false) {
            ControlBlock<T>* other_ctrl = other.ctrl_.load(std::memory_order_acquire);
            bool other_weak = other.is_weak_.load(std::memory_order_acquire);

            if (other_ctrl) {
                if (other_weak) {
                    other_ctrl->add_weak();
                }
                else {
                    other_ctrl->add_strong();
                }
            }
            ctrl_.store(other_ctrl, std::memory_order_release);
            is_weak_.store(other_weak, std::memory_order_release);
        }

        VSharedPtr(VSharedPtr&& other) noexcept
            : ctrl_(other.ctrl_.exchange(nullptr, std::memory_order_acq_rel)),
            is_weak_(other.is_weak_.exchange(false, std::memory_order_acq_rel)) {
        }

        template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
        VSharedPtr(const VSharedPtr<U>& other) noexcept : ctrl_(nullptr), is_weak_(false) {
            auto other_ctrl = other.ctrl_.load(std::memory_order_acquire);
            bool other_weak = other.is_weak_.load(std::memory_order_acquire);

            ControlBlock<T>* converted_ctrl = reinterpret_cast<ControlBlock<T>*>(other_ctrl);

            if (converted_ctrl) {
                if (other_weak) {
                    converted_ctrl->add_weak();
                }
                else {
                    converted_ctrl->add_strong();
                }
            }

            ctrl_.store(converted_ctrl, std::memory_order_release);
            is_weak_.store(other_weak, std::memory_order_release);
        }

        ~VSharedPtr() {
            release();
        }

        VSharedPtr& operator=(const VSharedPtr& other) noexcept {
            if (this != &other) {
                VSharedPtr tmp(other);
                swap(tmp);
            }
            return *this;
        }

        VSharedPtr& operator=(VSharedPtr&& other) noexcept {
            if (this != &other) {
                release();
                ctrl_.store(other.ctrl_.exchange(nullptr, std::memory_order_acq_rel),
                    std::memory_order_release);
                is_weak_.store(other.is_weak_.exchange(false, std::memory_order_acq_rel),
                    std::memory_order_release);
            }
            return *this;
        }

        VSharedPtr& operator=(std::nullptr_t) noexcept {
            reset();
            return *this;
        }

        VSharedPtr safe(const VSharedPtr& strong_ref) const {
            ControlBlock<T>* ref_ctrl = strong_ref.ctrl_.load(std::memory_order_acquire);
            bool ref_weak = strong_ref.is_weak_.load(std::memory_order_acquire);

            if (!ref_ctrl || ref_weak) {
                return VSharedPtr();
            }
            VSharedPtr weak_ptr;
            weak_ptr.ctrl_.store(ref_ctrl, std::memory_order_release);
            weak_ptr.is_weak_.store(true, std::memory_order_release);
            ref_ctrl->add_weak();
            return weak_ptr;
        }

        VSharedPtr lock() const {
            bool weak = is_weak_.load(std::memory_order_acquire);
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);

            if (!weak || !ctrl) {
                return VSharedPtr(*this);
            }
            if (ctrl->try_add_strong()) {
                return VSharedPtr(ctrl, false);
            }
            return VSharedPtr();
        }

        void Ref(const VSharedPtr& other) {
            if (this == &other) return;
            release();

            ControlBlock<T>* other_ctrl = other.ctrl_.load(std::memory_order_acquire);
            bool other_weak = other.is_weak_.load(std::memory_order_acquire);

            if (other_ctrl && !other_weak) {
                other_ctrl->add_weak();
                ctrl_.store(other_ctrl, std::memory_order_release);
                is_weak_.store(true, std::memory_order_release);
            }
        }

        bool expired() const noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            return !ctrl || !ctrl->is_alive();
        }

        T* get() const noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            if (!ctrl) {
                return nullptr;
            }
            bool weak = is_weak_.load(std::memory_order_acquire);
            if (weak) {
                return nullptr;
            }
            return ctrl->get_ptr();
        }

        T& operator*() const noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            bool weak = is_weak_.load(std::memory_order_acquire);
            assert(!weak && ctrl && "Dereferencing null");
            return *ctrl->get_ptr();
        }

        T* operator->() const noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            bool weak = is_weak_.load(std::memory_order_acquire);
            assert(!weak && ctrl && "Accessing through null");
            return ctrl->get_ptr();
        }

        explicit operator bool() const noexcept {
            bool weak = is_weak_.load(std::memory_order_acquire);
            if (weak) {
                return !expired();
            }
            return get() != nullptr;
        }

        size_t ref_count() const noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            return ctrl ? ctrl->strong_count() : 0;
        }

        size_t weak_count() const noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            return ctrl ? ctrl->weak_count() : 0;
        }

        bool unique() const noexcept {
            return ref_count() == 1;
        }

        bool is_weak() const noexcept {
            return is_weak_.load(std::memory_order_acquire);
        }

        void reset() noexcept {
            release();
            ctrl_.store(nullptr, std::memory_order_release);
            is_weak_.store(false, std::memory_order_release);
        }

        void reset(T* ptr) {
            VSharedPtr tmp(ptr);
            swap(tmp);
        }

        void swap(VSharedPtr& other) noexcept {
            ControlBlock<T>* my_ctrl = ctrl_.exchange(
                other.ctrl_.load(std::memory_order_acquire),
                std::memory_order_acq_rel);
            other.ctrl_.store(my_ctrl, std::memory_order_release);

            bool my_weak = is_weak_.exchange(
                other.is_weak_.load(std::memory_order_acquire),
                std::memory_order_acq_rel);
            other.is_weak_.store(my_weak, std::memory_order_release);
        }

        bool operator==(const VSharedPtr& other) const noexcept {
            return get() == other.get();
        }

        bool operator!=(const VSharedPtr& other) const noexcept {
            return !(*this == other);
        }

        bool operator==(std::nullptr_t) const noexcept {
            return get() == nullptr;
        }

        bool operator!=(std::nullptr_t) const noexcept {
            return get() != nullptr;
        }

    private:
        void release() noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            if (ctrl) {
                bool weak = is_weak_.load(std::memory_order_acquire);
                if (weak) {
                    ctrl->release_weak();
                }
                else {
                    ctrl->release_strong();
                }
            }
        }
    };

    // Specialization for array types T[]
    template<typename T>
    class VSharedPtr<T[]> {
    private:
        std::atomic<ControlBlock<T>*> ctrl_;
        std::atomic<bool> is_weak_;

        explicit VSharedPtr(ControlBlock<T>* ctrl, bool is_weak) noexcept
            : ctrl_(ctrl), is_weak_(is_weak) {
        }

        template<typename U> friend class VSharedPtr;

    public:
        constexpr VSharedPtr() noexcept : ctrl_(nullptr), is_weak_(false) {}
        constexpr VSharedPtr(std::nullptr_t) noexcept : ctrl_(nullptr), is_weak_(false) {}

        explicit VSharedPtr(T* ptr) : ctrl_(nullptr), is_weak_(false) {
            if (ptr) {
                try {
                    ctrl_.store(new ControlBlock<T>(ptr, true), std::memory_order_release);
                }
                catch (...) {
                    delete[] ptr;
                    throw;
                }
            }
        }

        VSharedPtr(const VSharedPtr& other) noexcept : ctrl_(nullptr), is_weak_(false) {
            ControlBlock<T>* other_ctrl = other.ctrl_.load(std::memory_order_acquire);
            bool other_weak = other.is_weak_.load(std::memory_order_acquire);

            if (other_ctrl) {
                if (other_weak) {
                    other_ctrl->add_weak();
                }
                else {
                    other_ctrl->add_strong();
                }
            }
            ctrl_.store(other_ctrl, std::memory_order_release);
            is_weak_.store(other_weak, std::memory_order_release);
        }

        VSharedPtr(VSharedPtr&& other) noexcept
            : ctrl_(other.ctrl_.exchange(nullptr, std::memory_order_acq_rel)),
            is_weak_(other.is_weak_.exchange(false, std::memory_order_acq_rel)) {
        }

        ~VSharedPtr() {
            release();
        }

        VSharedPtr& operator=(const VSharedPtr& other) noexcept {
            if (this != &other) {
                VSharedPtr tmp(other);
                swap(tmp);
            }
            return *this;
        }

        VSharedPtr& operator=(VSharedPtr&& other) noexcept {
            if (this != &other) {
                release();
                ctrl_.store(other.ctrl_.exchange(nullptr, std::memory_order_acq_rel),
                    std::memory_order_release);
                is_weak_.store(other.is_weak_.exchange(false, std::memory_order_acq_rel),
                    std::memory_order_release);
            }
            return *this;
        }

        VSharedPtr& operator=(std::nullptr_t) noexcept {
            reset();
            return *this;
        }

        VSharedPtr safe(const VSharedPtr& strong_ref) const {
            ControlBlock<T>* ref_ctrl = strong_ref.ctrl_.load(std::memory_order_acquire);
            bool ref_weak = strong_ref.is_weak_.load(std::memory_order_acquire);

            if (!ref_ctrl || ref_weak) {
                return VSharedPtr();
            }
            VSharedPtr weak_ptr;
            weak_ptr.ctrl_.store(ref_ctrl, std::memory_order_release);
            weak_ptr.is_weak_.store(true, std::memory_order_release);
            ref_ctrl->add_weak();
            return weak_ptr;
        }

        VSharedPtr lock() const {
            bool weak = is_weak_.load(std::memory_order_acquire);
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);

            if (!weak || !ctrl) {
                return VSharedPtr(*this);
            }
            if (ctrl->try_add_strong()) {
                return VSharedPtr(ctrl, false);
            }
            return VSharedPtr();
        }

        void Ref(const VSharedPtr& other) {
            if (this == &other) return;
            release();

            ControlBlock<T>* other_ctrl = other.ctrl_.load(std::memory_order_acquire);
            bool other_weak = other.is_weak_.load(std::memory_order_acquire);

            if (other_ctrl && !other_weak) {
                other_ctrl->add_weak();
                ctrl_.store(other_ctrl, std::memory_order_release);
                is_weak_.store(true, std::memory_order_release);
            }
        }

        bool expired() const noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            return !ctrl || !ctrl->is_alive();
        }

        T* get() const noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            if (!ctrl) {
                return nullptr;
            }
            bool weak = is_weak_.load(std::memory_order_acquire);
            if (weak) {
                return nullptr;
            }
            return ctrl->get_ptr();
        }

        // Array subscript operator
        T& operator[](size_t index) const noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            bool weak = is_weak_.load(std::memory_order_acquire);
            assert(!weak && ctrl && "Array access on null pointer");
            return ctrl->get_ptr()[index];
        }

        explicit operator bool() const noexcept {
            bool weak = is_weak_.load(std::memory_order_acquire);
            if (weak) {
                return !expired();
            }
            return get() != nullptr;
        }

        size_t ref_count() const noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            return ctrl ? ctrl->strong_count() : 0;
        }

        size_t weak_count() const noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            return ctrl ? ctrl->weak_count() : 0;
        }

        bool unique() const noexcept {
            return ref_count() == 1;
        }

        bool is_weak() const noexcept {
            return is_weak_.load(std::memory_order_acquire);
        }

        void reset() noexcept {
            release();
            ctrl_.store(nullptr, std::memory_order_release);
            is_weak_.store(false, std::memory_order_release);
        }

        void reset(T* ptr) {
            VSharedPtr tmp(ptr);
            swap(tmp);
        }

        void swap(VSharedPtr& other) noexcept {
            ControlBlock<T>* my_ctrl = ctrl_.exchange(
                other.ctrl_.load(std::memory_order_acquire),
                std::memory_order_acq_rel);
            other.ctrl_.store(my_ctrl, std::memory_order_release);

            bool my_weak = is_weak_.exchange(
                other.is_weak_.load(std::memory_order_acquire),
                std::memory_order_acq_rel);
            other.is_weak_.store(my_weak, std::memory_order_release);
        }

        bool operator==(const VSharedPtr& other) const noexcept {
            return get() == other.get();
        }

        bool operator!=(const VSharedPtr& other) const noexcept {
            return !(*this == other);
        }

        bool operator==(std::nullptr_t) const noexcept {
            return get() == nullptr;
        }

        bool operator!=(std::nullptr_t) const noexcept {
            return get() != nullptr;
        }

    private:
        void release() noexcept {
            ControlBlock<T>* ctrl = ctrl_.load(std::memory_order_acquire);
            if (ctrl) {
                bool weak = is_weak_.load(std::memory_order_acquire);
                if (weak) {
                    ctrl->release_weak();
                }
                else {
                    ctrl->release_strong();
                }
            }
        }
    };

    // ------------------------------------------------------------
// VMakeShared<T> for single objects
// ------------------------------------------------------------
    template<typename T, typename... Args>
    std::enable_if_t<!std::is_array_v<T>, VSharedPtr<T>>
        VMakeShared(Args&&... args) {
        return VSharedPtr<T>(new T(std::forward<Args>(args)...));
    }

    // ------------------------------------------------------------
    // VMakeShared<T[]> for arrays
    // ------------------------------------------------------------
    template<typename T>
    std::enable_if_t<std::is_array_v<T>, VSharedPtr<T>>
        VMakeShared(size_t count) {
        using Element = std::remove_extent_t<T>;
        return VSharedPtr<T>(new Element[count]);
    }


#define WEAK_ASSIGN(ptr, member, value) (ptr)->member.Ref(value)

}

//---------------------------------------------------------------------------------------------