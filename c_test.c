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