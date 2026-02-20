// meta.h : Include file for standard system include files,
#pragma once 

#ifdef __cplusplus
#include "cpp/Ptr.hpp"
#include "cpp/VSharedPtr.hpp"

extern "C" {
#endif

#include <stddef.h>

    // C-visible minimal struct (C++ expands it internally)
    typedef struct PtrBase {
        void* raw;
    } PtrBase;

    // Base allocators
    PtrBase new_malloc(size_t size);
    PtrBase new_calloc(size_t count, size_t size);

// Allocate one object of type T
#define New(T) \
    ((T*)new_malloc(sizeof(T)).raw)

// Allocate array (typed)
#define New_array(T, count) \
    ((T*)new_calloc((count), sizeof(T)).raw)

// malloc-style
#define New_malloc(size) \
    (new_malloc(size).raw)

// calloc-style
#define New_calloc(count, size) \
    (new_calloc((count), (size)).raw)


#ifdef __cplusplus
}
#endif