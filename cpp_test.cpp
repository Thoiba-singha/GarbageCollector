

#include "gc/gc.h"

#include <iostream>

struct Node {
    GC::Ptr<Node> next;
    GC::VSharedPtr<Node> prev;

    Node() {
        std::cout << "Node created\n";
    }
    ~Node() {
        std::cout << "Node destroyed\n";
    }
};

int main() {

    {
        // Cyclic dependency safe
        /*auto a = GC::New<Node>();
        auto b = GC::New<Node>();
        a->next = b;
        b->next = a;*/

        
        /*auto x = GC::VMakeShared<Node>();
        auto y = GC::VMakeShared<Node>();
        x->prev.Ref(y);
        y->prev.Ref(x);*/

        // for Array types
        auto arr = GC::VMakeShared<int[]>(5);
        for (int i = 0; i < 5; ++i)
            arr[i] = i * 10;
    }
}
