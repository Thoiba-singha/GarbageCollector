# 🗑️ Realtime C/C++ Garbage Collector

A lightweight, thread safe C/C++ garbage collector (GC)**.  

The GC is designed to be:  
- **Automatic** → cleanup followed by RAII principles and Global cleanup. 
- **Safe** → with inbuit cyclic ref safe without weak_ptr.  
- **Thread safe**.  

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
- `GC::VSharedPtr<T> || GC::VSharedPtr<T[]>` → similar to std::shared_ptr<> with build in cycle detection.
- `GC::VMakeShared<T> || GC::VMakeShared<T[]>` → similar to std::make_shared<>.
- `ref_count` → to count the current ref.
- `Ref` → Cyclic ref safe(No need weak_ptr).

---

**C - Example usage:**
```c
#include "gc/gc.h"
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

**C++ - Example usage:**
```c


#include "gc/gc.h"
#include <iostream>

struct Node {
    int data;
    GC::VSharedPtr<Node> next;

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
        GC::VSharedPtr<Node> node1(new Node(40));
        GC::VSharedPtr<Node> node2(new Node(50));
        node1->next.Ref(node2);
        node2->next.Ref(node1);
        std::cout << "Node1 use_count: " << node1.ref_count() << "\n\n";
        std::cout << "Node2 use_count: " << node2.ref_count() << "\n\n";

    }
    return 0;
}



```

**Importants**

- `realloc()` → not supported , the C APIs are fully writtern in C++ RAII principles.  
- `Warning!` → Do not mixed these APIs. 
- `Optimization` → not mature yet.
