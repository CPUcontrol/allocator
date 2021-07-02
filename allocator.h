#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct Enj_Allocator{
    void *  (*alloc)(size_t, void *);
    void    (*dealloc)(void *, void *);
    void     *data;
} Enj_Allocator;

typedef struct Enj_BumpAllocatorData{
    void *start;
    size_t size;
    void *head;
} Enj_BumpAllocatorData;
typedef struct Enj_StackAllocatorData{
    void *start;
    size_t size;
    void *head;
} Enj_StackAllocatorData;
typedef struct Enj_PoolAllocatorData{
    void *start;
    size_t size;
    size_t chunksize;

    void *free;
} Enj_PoolAllocatorData;
typedef struct Enj_HeapAllocatorData{
    void *start;
    size_t size;

    void *root;
} Enj_HeapAllocatorData;

void * Enj_Alloc(Enj_Allocator *a, size_t size);
void Enj_Free(Enj_Allocator *a, void *p);


void Enj_InitBumpAllocator(
    Enj_Allocator *a, 
    Enj_BumpAllocatorData *d,
    void *buffer, 
    size_t size);

void Enj_InitStackAllocator(
    Enj_Allocator *a, 
    Enj_StackAllocatorData *d,
    void *buffer, 
    size_t size);

void Enj_InitPoolAllocator(
    Enj_Allocator *a, 
    Enj_PoolAllocatorData *d,
    void *buffer, 
    size_t size,
    size_t chunksize);

void Enj_InitHeapAllocator(
    Enj_Allocator *a, 
    Enj_HeapAllocatorData *d,
    void *buffer, 
    size_t size);

#ifdef __cplusplus
}
#endif