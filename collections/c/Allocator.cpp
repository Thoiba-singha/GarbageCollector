#include "../meta.h"
#include <memory>
#include <iostream>
#include <cstring>
#include <unordered_map>
#include <mutex>

// for debug purposes only
struct DebugDeleter {
    void operator()(char* ptr) const noexcept {
        std::cout << "[C++ backend] Freed memory @ "
            << static_cast<void*>(ptr) << "\n";
        delete[] ptr;
    }
};

// Global registry to track shared_ptr for each allocation
static std::unordered_map<void*, std::shared_ptr<char>> g_ptr_registry;
static std::mutex g_registry_mutex;

// The real object with destructor (invisible to C)
struct LocalPtrCpp {
    void* raw = nullptr;
    std::shared_ptr<char> holder;

    ~LocalPtrCpp() {
        // When this dies → shared_ptr dies → DebugDeleter prints
        // Also clean up from registry
        if (raw) {
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            g_ptr_registry.erase(raw);
        }
    }
};

extern "C" {
    // allocate size bytes
    PtrBase new_malloc(size_t size) {
        LocalPtrCpp cpp;
        cpp.holder = std::shared_ptr<char>(new char[size], DebugDeleter{});
        cpp.raw = cpp.holder.get();

        // Register in global map
        {
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            g_ptr_registry[cpp.raw] = cpp.holder;
        }

        return *(PtrBase*)&cpp;
    }

    // allocate and zero memory
    PtrBase new_calloc(size_t count, size_t size) {
        size_t total = count * size;
        LocalPtrCpp cpp;
        cpp.holder = std::shared_ptr<char>(new char[total], DebugDeleter{});
        std::memset(cpp.holder.get(), 0, total);
        cpp.raw = cpp.holder.get();

        // Register in global map
        {
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            g_ptr_registry[cpp.raw] = cpp.holder;
        }

        return *(PtrBase*)&cpp;
    }

} // extern "C"