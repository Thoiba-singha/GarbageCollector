# 🗑️ Realtime C/C++ Garbage Collector + Thread-Safe Smart pointer

A lightweight, thread safe C/C++ garbage collector (GC)**.  

The GC is designed to be:  
- **Automatic** → cleanup followed by RAII principles and Global cleanup. 
- **weak function** → with inbuit cyclic ref safe without weak_ptr.  
- **Thread safe VSharedPtr<T>** → with inbuit ThreadMode::True.

---

- **Allocation APIs in C**  
- `New_malloc` → like malloc().  
- `New_calloc` → zero-initialized allocation like calloc().  
- `New_array_` → for array.
- `New` → single object.

---

- **Allocation APIs in C++**  
- `GC::Ptr<T> || GC::Ptr<T[]>` → A Global `Ptr class` for GC.  
- `GC::New<T> || GC::New<T[]>` → Factory function(). 
- `ptr::VSharedPtr<T> || ptr::VSharedPtr<T[]>` → similar to std::shared_ptr<> with build in cycle detection and Extra ThreadMode.
- `ptr::VMakeShared<T> || ptr::VMakeShared<T[]>` → Factory function.
- `ref_count` → to count the current ref.
- `weak` → Cyclic ref safe(No need weak_ptr).

---

**C - Example usage:**

```c
#include "collections/meta.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    int x;
    float y;
    struct Node* next;

} Node;

int main() {
    {
        Node* n1 = New_malloc(sizeof(Node));
        Node* n2 = New_malloc(sizeof(Node));
        // Create a cycle

        n1->next = n2;
        n2->next = n1;

        printf("Cycle created:\n");
        printf("  n1->next = %p\n", (void*)n1->next);
        printf("  n2->next = %p\n", (void*)n2->next);

        //// Break the cycle FIRST
        n2->next = NULL;
        n1->next = NULL;

        // Debug: verify pointers are NULL
        printf("After break:\n");
        printf("  n1->next = %p (should be NULL)\n", (void*)n1->next);
        printf("  n2->next = %p (should be NULL)\n", (void*)n2->next);

    }

    printf("Exiting\n");

    return 0;
}


```

**C++ VSharedPtr<T> - Example usage:**
```c


#include "collections/meta.h"
#include <iostream>

struct Node {
    int data;
    ptr::VSharedPtr<Node> next;

    explicit Node(int d) : data(d) {
        std::cout << "Node(" << data << ") created\n";
    }

    ~Node() {
        std::cout << "Node(" << data << ") destroyed\n";
    }
};

int main() {
    //  Basic cyclic dependency test
    {
        ptr::VSharedPtr<Node> node1(new Node(40));
        ptr::VSharedPtr<Node> node2(new Node(50));
        node1->next.weak(node2);
        node2->next.weak(node1);
        std::cout << "Node1 use_count: " << node1.ref_count() << "\n\n";
        std::cout << "Node2 use_count: " << node2.ref_count() << "\n\n";

    }
    return 0;
}


```
**C++ VSharedPtr<T> Thread Safe - Example usage:**

```c
  
   ptr::VSharedPtr<Node, ptr::meta::ThreadMode::True> a(new Node); // Completly Threadsafe both ref and object level
   ptr::VSharedPtr<Node, ptr::meta::ThreadMode::False> b(new Node);

```
**Importants**

- `realloc()` → not supported , the C APIs are fully writtern in C++ RAII principles. 
- `ThreadMode` → only Available for VSharedPtr.
- `Warning!` → Do not mixed these APIs. 
- `Optimization` → not mature yet.
