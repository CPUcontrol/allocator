#include "allocator.h"

/*ALIGN_SIZE must be a power of 2*/
#define ALIGN_SIZE 16
#define ROUNDUP(n, m) (((n) + (m) - 1) / (m) * (m))
#define ROUNDDOWN(n, m) ((n) / (m) * (m))
#define ROUND_PTR(i) ROUNDUP(i, sizeof(void *))

static void * bump_acate(size_t size, void *data);
static void bump_decate(void *p, void *data);

static void * stack_acate(size_t size, void *data);
static void stack_decate(void *p, void *data);

static void * pool_acate(size_t size, void *data);
static void pool_decate(void *p, void *data);

static void * heap_acate(size_t size, void *data);
static void heap_decate(void *p, void *data);

void * Enj_Alloc(Enj_Allocator *a, size_t size){
    return (*a->alloc)(size, a->data);
}
void Enj_Free(Enj_Allocator *a, void *p){
    (*a->dealloc)(p, a->data);
}

void Enj_InitBumpAllocator(
    Enj_Allocator *a,
    Enj_BumpAllocatorData *d,
    void *buffer,
    size_t size){
    a->alloc = &bump_acate;
    a->dealloc = &bump_decate;
    a->data = d;
    d->start = buffer;
    d->size = size;
    d->head = buffer;
}

void Enj_InitStackAllocator(
    Enj_Allocator *a,
    Enj_StackAllocatorData *d,
    void *buffer,
    size_t size){
    a->alloc = &stack_acate;
    a->dealloc = &stack_decate;
    a->data = d;
    d->start = buffer;
    d->size = size;
    d->head = buffer;
}

void Enj_InitPoolAllocator(
    Enj_Allocator *a,
    Enj_PoolAllocatorData *d,
    void *buffer,
    size_t size,
    size_t chunksize){

    size_t align;

    char *i;
    char *end;

    a->alloc = &pool_acate;
    a->dealloc = &pool_decate;
    a->data = d;

    /*chunksize at least twice pointer size for pointer alignment*/
    align = chunksize <= 2*sizeof(void *) ?
        ROUND_PTR(chunksize) :
        chunksize;

    d->start = buffer;
    d->size = size;
    d->chunksize = chunksize;

    d->free = buffer;
    /*If no room for a single chunk, edge case*/
    if(align > d->size){
        d->free = NULL;
        return;
    }

    /*Initialize pool chunks*/
    i = (char *)d->free;
    end = (char *)d->start + d->size - align*2;
    /*Iterate over all chunks, adding the next one to free list*/
    /*Pointer is aligned in chunk by rounding up*/
    while(i <= end){
        *(void **)((char *)d->free + ROUND_PTR(i-(char *)d->free))
            = (void *)(i + align);
        i = i + align;
    }
    /*Last element in pool points to NULL to end the list*/
    *(void **)i = NULL;

}

typedef struct heap_header{
    size_t prev_alloc;
    size_t next_color;
} heap_header;

struct heap_free;
typedef struct heap_free heap_free;
struct heap_free{
    heap_header header;

    heap_free *chs[2]; /*Indices 0, 1 mean left, right children*/
    heap_free *parent;

    /*Either NULL or points to double-linked list*/
    /*Points to self instead if part of list*/
    heap_free *duplist;
};

void Enj_InitHeapAllocator(
    Enj_Allocator *a,
    Enj_HeapAllocatorData *d,
    void *buffer,
    size_t size){

    size_t space;

    heap_free *r;
    heap_header *end;

    a->alloc = &heap_acate;
    a->dealloc = &heap_decate;
    a->data = d;

    d->start = buffer;
    d->size = size;

    space = ROUNDDOWN(size, ALIGN_SIZE);
    /*Stop if not enough space*/
    if(space <
      ROUNDUP(sizeof(heap_free), ALIGN_SIZE)
    + ROUNDUP(sizeof(heap_header), ALIGN_SIZE)
    ){
        d->root = NULL;
        return;
    }

    d->root = buffer;

    r = (heap_free *)d->root;



    end = (heap_header *)((char *)buffer
        + ROUNDDOWN(
        size - sizeof(heap_header),
        ALIGN_SIZE));

    r->header.prev_alloc = 0;
    r->header.next_color =
        ROUNDDOWN(size - sizeof(heap_header),
                ALIGN_SIZE);
    r->chs[0] = NULL;
    r->chs[1] = NULL;
    r->parent = NULL;
    r->duplist = NULL;

    end->prev_alloc = ROUNDDOWN(size - sizeof(heap_header),
                ALIGN_SIZE) | 1;
    end->next_color = 0;


}

static void * bump_acate(size_t size, void *data){
    Enj_BumpAllocatorData *stack = (Enj_BumpAllocatorData *)data;

    /*Round up added size*/
    size_t roundupsize = (size + ALIGN_SIZE - 1) / ALIGN_SIZE * ALIGN_SIZE;

    void *res;

    /*Check if enough room*/
    if((char *)stack->head  + roundupsize
    > (char *)stack->start + stack->size){
        return NULL;
    }

    res = stack->head;
    stack->head = (void *)((char *)stack->head + roundupsize);

    return res;
}

static void bump_decate(void *p, void *data){
    /*Do nothing, not supposed to deallocate individual chunks.*/
    return;
}

static void * stack_acate(size_t size, void *data){
    Enj_StackAllocatorData *stack = (Enj_StackAllocatorData *)data;

    /*Round up added size*/
    size_t roundupsize = (size + ALIGN_SIZE - 1) / ALIGN_SIZE * ALIGN_SIZE;

    void *res;

    /*Check if enough room*/
    if((char *)stack->head + roundupsize
    > (char *)stack->start + stack->size){
        return NULL;
    }

    res = stack->head;
    stack->head = (void *)((char *)stack->head + roundupsize);

    return res;
}
static void stack_decate(void *p, void *data){
    Enj_StackAllocatorData *stack;

    if (!p) return;

    stack = (Enj_StackAllocatorData *)data;
    stack->head = p;
}

static void * pool_acate(size_t size, void *data){
    Enj_PoolAllocatorData *pool = (Enj_PoolAllocatorData *)data;

    void *res;

    /*No more room*/
    if((pool->chunksize != size) | (!pool->free)){
        return NULL;
    }

    res = pool->free;
    pool->free = *(void **)((char *)pool->start
    + ROUND_PTR((char *)pool->free - (char *)pool->start));

    return res;
}
static void pool_decate(void *p, void *data){
    Enj_PoolAllocatorData *pool;

    if (!p) return;

    pool = (Enj_PoolAllocatorData *)data;

    *(void **)((char *)pool->start
    + ROUND_PTR((char *)p - (char *)pool->start)) = pool->free;
    pool->free = p;
}



/*RB Tree and heap stuff*/

static void freerotate(Enj_HeapAllocatorData *h, heap_free *f, int dir){
    heap_free *c = f->chs[1 ^ dir];

    if (f->parent) {
        f->parent->chs[f->parent->chs[1] == f] = c;
    }
    else h->root = c;

    c->parent = f->parent;

    f->chs[1 ^ dir] = c->chs[dir];
    if(c->chs[dir]) c->chs[dir]->parent = f;

    f->parent = c;
    c->chs[dir] = f;
}

static heap_free * findbestfree(Enj_HeapAllocatorData *h, size_t size){
    heap_free *it = (heap_free *)h->root;
    heap_free *best = NULL;
    while (it){
        size_t space = (it->header.next_color & ~1)
            - ROUNDUP(sizeof(heap_header), ALIGN_SIZE);
        /*Go left if sufficient space, right otherwise, if exact then return*/
        if (space == size){
            return it;
        }
        else if (space > size){
            best = it;
            it = it->chs[0];
        }
        else{
            it = it->chs[1];
        }
    }

    return best;
}

static void removefree_tree(Enj_HeapAllocatorData *h, heap_free *f){

    heap_free placeholder;
    heap_free *db;

    heap_free *u;

    if (f->duplist){
        /*Replace node from duplist instead of removing from tree*/
        heap_free *replacement = f->duplist;

        /*Update field of replacement with old node's tree state*/
        replacement->duplist = replacement->chs[1];
        replacement->parent = f->parent;
        replacement->chs[0] = f->chs[0];
        replacement->chs[1] = f->chs[1];
        replacement->header.next_color =
            (replacement->header.next_color & ~1)
            | (f->header.next_color & 1);

        /*Update other nodes in tree to point to replacement*/
        if (f->parent) {
            if(f == f->parent->chs[0]) f->parent->chs[0] = replacement;
            else f->parent->chs[1] = replacement;
        }
        else {
            h->root = replacement;
        }
        if (f->chs[0]) f->chs[0]->parent = replacement;
        if (f->chs[1]) f->chs[1]->parent = replacement;

        return;
    }

    if ((f == h->root) & (f->chs[0] == NULL) & (f->chs[1] == NULL)){
        h->root = NULL;

        return;
    }



    placeholder.header.next_color = 0;
    placeholder.parent = NULL;
    placeholder.chs[0] = NULL;
    placeholder.chs[1] = NULL;

    /*Both children*/
    if((f->chs[0] != NULL) & (f->chs[1] != NULL)){
        int f_red;
        int it_red;
        heap_free *swap;
        /*Exchange f with inorder successor*/
        heap_free *it = f->chs[1];
        while(it->chs[0]){
            it = it->chs[0];
        }

        f_red = f->header.next_color & 1;
        it_red = it->header.next_color & 1;

        it->header.next_color =
            (it->header.next_color&~1) | f_red;
        f->header.next_color =
            (f->header.next_color&~1) | it_red;

        if (it->chs[0]) it->chs[0]->parent = f;
        if (it->chs[1]) it->chs[1]->parent = f;
        if (f->chs[0]) f->chs[0]->parent = it;
        if (f->chs[1]) f->chs[1]->parent = it;

        swap = it->chs[0];
        it->chs[0] = f->chs[0];
        f->chs[0] = swap;
        swap = it->chs[1];
        it->chs[1] = f->chs[1];
        f->chs[1] = swap;

        if(f->parent){
            if(f == f->parent->chs[0]) f->parent->chs[0] = it;
            else f->parent->chs[1] = it;
        }
        else {
            /*change root*/
            h->root = it;
        }

        if(it == it->parent->chs[0]) it->parent->chs[0] = f;
        else it->parent->chs[1] = f;

        swap = it->parent;
        it->parent = f->parent;
        f->parent = swap;

    }
    /*Only left child*/
    if(f->chs[0] != NULL){
        f->chs[0]->header.next_color &= ~1;

        f->chs[0]->parent = f->parent;
        if(f->parent){
            if(f == f->parent->chs[0]) f->parent->chs[0] = f->chs[0];
            else f->parent->chs[1] = f->chs[0];
        }
        else {
            /*change root*/
            h->root = f->chs[0];
        }

        return;
    }
    /*Only right child*/
    if(f->chs[1] != NULL){
        f->chs[1]->header.next_color &= ~1;

        f->chs[1]->parent = f->parent;
        if(f->parent){
            if(f == f->parent->chs[0]) f->parent->chs[0] = f->chs[1];
            else f->parent->chs[1] = f->chs[1];
        }
        else {
            /*change root*/
            h->root = f->chs[1];
        }

        return;
    }
    /*Zero children*/
    if(f->header.next_color & 1){
        if(f->parent){
            if(f == f->parent->chs[0]) f->parent->chs[0] = NULL;
            else f->parent->chs[1] = NULL;
        }
        else {
            /*change root*/
            h->root = NULL;
        }

        return;
    }
    else{
        db = &placeholder;

        db->parent = f->parent;
        if(f->parent){
            if(f == f->parent->chs[0]) f->parent->chs[0] = db;
            else f->parent->chs[1] = db;
        }
    }


    /*Rotations*/
    u = db;


    for (;;) {
        heap_free *s;

        int sleft_red;
        int sright_red;
        int u_side;

        /*C2*/
        if (!u->parent) break;

        u_side = (u == u->parent->chs[1]);
        s = u->parent->chs[!u_side];

        /*C3*/
        if (s->header.next_color & 1) {
            u->parent->header.next_color |= 1;
            s->header.next_color &= ~1;

            freerotate(h, u->parent, u_side);
            s = u->parent->chs[!u_side];
        }

        /*Check sibling children colors*/
        sleft_red
            = s->chs[0] ? (s->chs[0]->header.next_color & 1) : 0;
        sright_red
            = s->chs[1] ? (s->chs[1]->header.next_color & 1) : 0;
        /*C1*/
        if ((~u->parent->header.next_color & 1) & (~s->header.next_color & 1)
            & (1^sleft_red)
            & (1^sright_red)) {

            s->header.next_color |= 1;
            u = u->parent;


            continue;
        }
        /*C4*/
        if ((u->parent->header.next_color & 1) & (~s->header.next_color & 1)
            & (1^sleft_red)
            & (1^sright_red)) {

            s->header.next_color |= 1;
            u->parent->header.next_color &= ~1;

            break;
        }
        /*C5*/
        if ((~s->header.next_color & 1)) {
            if ((!u_side ^ sright_red) & (u_side ^ sleft_red)) {
                s->header.next_color |= 1;
                s->chs[u_side]->header.next_color &= ~1;

                freerotate(h, s, !u_side);

                s = s->parent;
            }
        }
        /*C6*/
        s->header.next_color = (s->header.next_color & ~1)
            | (u->parent->header.next_color & 1);
        u->parent->header.next_color &= ~1;

        s->chs[!u_side]->header.next_color &= ~1;
        freerotate(h, u->parent, u_side);

        break;
    }


    /*Delete placeholder*/
    if (placeholder.parent) {
        if (placeholder.parent->chs[0] == &placeholder)
            placeholder.parent->chs[0] = NULL;
        else placeholder.parent->chs[1] = NULL;
    }
}
static void insertfree(Enj_HeapAllocatorData *h, heap_free *f){

    heap_free *it;
    size_t space;

    if (!h->root){
        h->root = f;
        f->header.next_color &= ~1;
        f->parent = NULL;
        f->chs[0] = NULL;
        f->chs[1] = NULL;
        f->duplist = NULL;
        return;
    }

    it = (heap_free *)h->root;
    space = f->header.next_color & ~1;

    for(;;){
        size_t itspace = it->header.next_color & ~1;

        if(space == itspace){
            /*Insert into duplist instead of into tree*/

            f->chs[0] = it;
            f->chs[1] = it->duplist;
            f->parent = NULL;
            f->duplist = f;

            if (it->duplist) it->duplist->chs[0] = f;

            it->duplist = f;
            return;
        }
        else{
            if (it->chs[space > itspace]){
                it = it->chs[space > itspace];
            }
            else{
                it->chs[space > itspace] = f;
                f->parent = it;
                break;
            }
        }
    }

    f->chs[0] = NULL;
    f->chs[1] = NULL;
    f->duplist = NULL;
    f->header.next_color |= 1;

    for(;;){
        if (!f->parent) {
            /*Case 3*/
            f->header.next_color &= ~1;
            break;
        }
        else if((~f->parent->header.next_color & 1)){
            /*Case 1*/
            break;
        }
        else{
            heap_free *p = f->parent;
            if(!p->parent){
                /*Case 4*/
                p->header.next_color &= ~1;
                f->header.next_color |= 1;
                break;
            }
            else{
                heap_free *gp = p->parent;
                heap_free *u = gp->chs[p != gp->chs[1]];
                if(u && (u->header.next_color & 1)){
                    /*Case 2*/
                    p->header.next_color &= ~1;
                    u->header.next_color &= ~1;
                    gp->header.next_color |= 1;

                    f = gp;
                }
                else{
                    /*Case 5 & 6*/
                    if((f == p->chs[1]) ^ (p == gp->chs[1])){
                        freerotate(h, p, p == gp->chs[1]);
                        f = p;
                    }

                    p = f->parent;
                    gp = f->parent->parent;

                    freerotate(h, gp, f != p->chs[1]);
                    p->header.next_color &= ~1;
                    gp->header.next_color |= 1;

                    break;
                }
            }
        }

    }

}

static void removefree(Enj_HeapAllocatorData *h, heap_free *f){
    if(f->duplist == f){
        /*Remove from duplist*/
        heap_free *prev = f->chs[0];
        heap_free *next = f->chs[1];
        /*Head of duplist points back to tree node*/
        if (prev->duplist == prev) prev->chs[1] = next;
        else prev->duplist = next;

        if (next) next->chs[0] = prev;
    }
    else{
        removefree_tree(h, f);
    }
}

static void * heap_acate(size_t size, void *data){
    Enj_HeapAllocatorData *heap = (Enj_HeapAllocatorData *)data;
    size_t sizeround;
    size_t minsize;

    heap_free *bestfree;

    heap_header *head;
    void *res;
    size_t freespace;

    if(!heap->root) return NULL;

    /*find best fit out of tree*/
    /*either use entire free block, or split and add remainder back to tree*/

    sizeround = ROUNDUP(size, ALIGN_SIZE);

    minsize = ROUNDUP(
        sizeof(heap_free)-sizeof(heap_header), ALIGN_SIZE);
    if (minsize > sizeround) sizeround = minsize;

    bestfree = findbestfree(heap, sizeround);

    if(!bestfree) return NULL;
    removefree_tree(heap, bestfree);
    head = (heap_header *)bestfree;
    res = (void *)(
                (char *)head + ROUNDUP(sizeof(heap_header),ALIGN_SIZE));

    freespace =
        (head->next_color & ~1) - ROUNDUP(sizeof(heap_header),ALIGN_SIZE);


    if(freespace - sizeround > ROUNDUP(sizeof(heap_free),ALIGN_SIZE)){
        heap_free *newfree = (heap_free *)
            ((char *)res + sizeround);

        /*Replace node in list with new free block*/
        newfree->header.prev_alloc =
            sizeround + ROUNDUP(sizeof(heap_header),ALIGN_SIZE);
        newfree->header.next_color =
            head->next_color - sizeround
            - ROUNDUP(sizeof(heap_header),ALIGN_SIZE);

        ((heap_header *)((char *)head + (head->next_color & ~1)))->prev_alloc
            -= sizeround + ROUNDUP(sizeof(heap_header), ALIGN_SIZE);

        head->next_color = sizeround
            + ROUNDUP(sizeof(heap_header),ALIGN_SIZE);


        insertfree(heap, newfree);
    }
    /*Set block to allocated*/
    head->prev_alloc |= 1;

    return res;

}
static void heap_decate(void *p, void *data){
    Enj_HeapAllocatorData *heap;
    heap_free *newfree;

    if (!p) return;

    heap = (Enj_HeapAllocatorData *)data;
    newfree = (heap_free *)
        ((char *)p - ROUNDUP(sizeof(heap_header), ALIGN_SIZE));

    /*Check if next block is free to merge*/
    if (!(((heap_header *)
    ((char *)newfree + (newfree->header.next_color & ~1)))->prev_alloc & 1)){

        heap_header *oldn;
        heap_free *oldfree = (heap_free *)
            ((char *)newfree + (newfree->header.next_color & ~1));
        removefree(heap, oldfree);
        /*reconnect double list, bridging over oldfree*/
        newfree->header.next_color += oldfree->header.next_color & ~1;
        /*connect block right of oldfree to newfree*/
        oldn = (heap_header *)
            ((char *)oldfree + (oldfree->header.next_color & ~1));
        oldn->prev_alloc += oldfree->header.prev_alloc & ~1;
    }
    /*Check if previous block is free to merge*/

    if ((newfree->header.prev_alloc & ~1) && !(((heap_header *)
    ((char *)newfree - (newfree->header.prev_alloc & ~1)))->prev_alloc & 1)){

        heap_header *newn;
        heap_free *oldfree = (heap_free *)
            ((char *)newfree - (newfree->header.prev_alloc & ~1));
        removefree(heap, oldfree);
        /*connect oldfree to block to the right of newfree*/
        oldfree->header.next_color += newfree->header.next_color & ~1;

        /*connect block right of newfree to oldfree behind*/
        newn = (heap_header *)
            ((char *)newfree + (newfree->header.next_color & ~1));
        newn->prev_alloc += newfree->header.prev_alloc & ~1;
        newfree = oldfree;
    }

    newfree->header.prev_alloc &= ~1;
    insertfree(heap, newfree);
    return;
}
