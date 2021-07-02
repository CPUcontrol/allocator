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

    /*chunksize at least 16 for pointer alignment*/
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

    heap_free *left;
    heap_free *right;
    heap_free *parent;
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
    r->left = NULL;
    r->right = NULL;
    r->parent = NULL;

    end->prev_alloc = ROUNDDOWN(size - sizeof(heap_header), 
                ALIGN_SIZE) | 1;
    end->next_color = 0; 
    

}

static void * bump_acate(size_t size, void *data){
    Enj_BumpAllocatorData *stack = (Enj_BumpAllocatorData *)data;
    
    /*Round up added size - this works because ALIGN_SIZE is power of 2*/
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
    
    /*Round up added size - this works because ALIGN_SIZE is power of 2*/
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

static heap_free * freeuncle(heap_free *f){
    heap_free *p = f->parent;
    heap_free *gp = p->parent;

    size_t memboff = 
        p == gp->left 
        ? offsetof(heap_free, right) 
        : offsetof(heap_free, left);

    return *(heap_free **)((char *)gp + memboff);
}

static void freerotateleft(Enj_HeapAllocatorData* h, heap_free *f){
    heap_free *c = f->right;
    
    if (f->parent) {

        size_t memboff = 
            f->parent->left == f
            ? offsetof(heap_free, left) 
            : offsetof(heap_free, right);

        *(heap_free **)((char *)f->parent + memboff) = c;
    }
    else h->root = c;

    c->parent = f->parent;

    f->right = c->left;
    if(c->left) c->left->parent = f;

    f->parent = c; 
    c->left = f;
}

static void freerotateright(Enj_HeapAllocatorData* h, heap_free *f){
    heap_free *c = f->left;

    if (f->parent) {

        size_t memboff = 
            f->parent->left == f
            ? offsetof(heap_free, left) 
            : offsetof(heap_free, right);

        *(heap_free **)((char *)f->parent + memboff) = c;
    }
    else h->root = c;

    c->parent = f->parent;

    f->left = c->right;
    if(c->right) c->right->parent = f;

    f->parent = c;
    c->right = f;
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
            it = it->left;
        }
        else{
            it = it->right;
        }
    }

    return best;
}
static void removefree(Enj_HeapAllocatorData *h, heap_free *f){

    heap_free placeholder;
    heap_free *db;
    unsigned char childmask;

    heap_free *u;

    if ((f == h->root) & (f->left == NULL) & (f->right == NULL)){
        h->root = NULL;

        return;
    }


    
    placeholder.header.next_color = 0;
    placeholder.parent = NULL;
    placeholder.left = NULL;
    placeholder.right = NULL;

    
    /*Cases of how many children f has*/
    /*if either node being removed or replacement node red, end early*/
    childmask = (f->left != NULL)<<1 | (f->right != NULL);

    /*No children*/
    switch(childmask){
    case 0:{
        if(f->header.next_color & 1){
            if(f->parent){
                if(f == f->parent->left) f->parent->left = NULL;
                else f->parent->right = NULL;
            }

            return;
        }

        db = &placeholder;

        placeholder.parent = f->parent;
        if(f->parent){
            if(f == f->parent->left) f->parent->left = db;
            else f->parent->right = db;

        }


    }
    break;
    /*Only right child*/
    case 1:{
        int red = (f->header.next_color | f->right->header.next_color) & 1;

        db = f->right;

        f->right->header.next_color &= ~1;

        f->right->parent = f->parent;
        if(f->parent){
            if(f == f->parent->left) f->parent->left = f->right;
            else f->parent->right = f->right;
        }
        else {
            /*change root*/
            h->root = f->right;
        }

        if(red) return;

    }
    break;
    /*Only left child*/
    case 2:{
        int red = (f->header.next_color | f->left->header.next_color) & 1;

        db = f->left;

        f->left->header.next_color &= ~1;

        f->left->parent = f->parent;
        if(f->parent){
            if(f == f->parent->left) f->parent->left = f->left;
            else f->parent->right = f->left;
        }
        else {
            /*change root*/
            h->root = f->left;
        }

        if(red) return;

    }
    break;
    /*Both children*/
    case 3:{
        int red_succ;
        heap_free* succparent;
        heap_free* succright;

        /*Replace f with inorder successor*/
        heap_free *it = f->right;
        while(it->left){
            it = it->left;
        }
        red_succ = it->header.next_color & 1;

        succparent = it->parent;
        succright = it->right;

        /*Make the it node new parent of f children*/
        it->header.next_color = 
            (it->header.next_color&~1) | (f->header.next_color&1);

        it->parent = f->parent;
        if(f->parent){
            if(f == f->parent->left) f->parent->left = it;
            else f->parent->right = it;
        }
        else {
            /*change root*/
            h->root = it;
        }

        
        f->left->parent = it;
        it->left = f->left;
        /*Special case if successor is child of f node*/
        if (f->right == it) {
            if (red_succ) {
                /*Nothing to do, successor was originally red*/
                return;
            }

            if (it->right) {
                db = it->right;
                if (db->header.next_color & 1) {
                    db->header.next_color &= ~1;

                    return;
                };
            }
            else {

                db = &placeholder;
                placeholder.parent = it;
                it->right = &placeholder;
            }
        }
        else {
            f->right->parent = it;
            it->right = f->right;

            /*If right child exists, make that d-black, else the placeholder*/
            if (succright) {
                succright->parent = succparent;
                succparent->left = succright;
                if (red_succ) {
                    return;
                }

                db = succright;
                if (db->header.next_color & 1) {
                    db->header.next_color &= ~1;

                    return;
                };
            }
            else{
                if(red_succ){
                    succparent->left = NULL;

                    return;
                }

                db = &placeholder;
                placeholder.parent = succparent;
                succparent->left = &placeholder;
            }
        }

    }
    break;
    }

    /*Rotations*/
    u = db;


    for (;;) {
        heap_free* s;

        unsigned char sleft_red;
        unsigned char sright_red;

        /*C1*/
        if (!u->parent) break;

        s = u == u->parent->right 
                    ? u->parent->left : u->parent->right;
        /*C2*/
        if (s->header.next_color & 1) {
            u->parent->header.next_color |= 1;
            s->header.next_color &= ~1;
            if (u == u->parent->left) {
                freerotateleft(h, u->parent);
                s = u->parent->right;
            }
            else {
                freerotateright(h, u->parent);
                s = u->parent->left;
            }

            
        }

        /*Check sibling children colors*/
        sleft_red 
            = s->left ? (s->left->header.next_color & 1) : 0;
        sright_red 
            = s->right ? (s->right->header.next_color & 1) : 0;
        /*C3*/
        if (!(u->parent->header.next_color & 1) & !(s->header.next_color & 1)
            & !sleft_red 
            & !sright_red) {

            s->header.next_color |= 1;
            u = u->parent;


            continue;
        }
        /*C4*/
        if ((u->parent->header.next_color & 1) & !(s->header.next_color & 1)
            & !sleft_red
            & !sright_red) {

            s->header.next_color |= 1;
            u->parent->header.next_color &= ~1;

            break;
        }
        /*C5*/
        if (!(s->header.next_color & 1)) {
            if ((u == u->parent->left) 
                & !sright_red
                & sleft_red) {
                s->header.next_color |= 1;
                s->left->header.next_color &= ~1;

                freerotateright(h, s);

                s = s->parent;
            }
            else if ((u == u->parent->right) 
                & !sleft_red
                & sright_red) {
                s->header.next_color |= 1;
                s->right->header.next_color &= ~1;

                freerotateleft(h, s);

                s = s->parent;
            }
        }

        s->header.next_color = (s->header.next_color & ~1) 
            | (u->parent->header.next_color & 1);
        u->parent->header.next_color &= ~1;

        if (u == u->parent->left) {
            s->right->header.next_color &= ~1;
            freerotateleft(h, u->parent);
        }
        else {
            s->left->header.next_color &= ~1;
            freerotateright(h, u->parent);
        }

        break;
    }
    

    /*Delete placeholder*/
    if (placeholder.parent && placeholder.parent->left == &placeholder)
        placeholder.parent->left = NULL;
    else if (placeholder.parent && placeholder.parent->right == &placeholder)
        placeholder.parent->right = NULL;



}
static void insertfree(Enj_HeapAllocatorData *h, heap_free *f){

    heap_free *it;
    size_t space;

    if (!h->root){
        h->root = f;
        f->header.next_color &= ~1;
        f->parent = NULL;
        f->left = NULL;
        f->right = NULL;
        return;
    }

    it = (heap_free *)h->root;
    space = (f->header.next_color & ~1)
        /*- ROUNDUP(sizeof(heap_header), ALIGN_SIZE)*/;

    for(;;){
        size_t itspace = (it->header.next_color & ~1)
            /*- ROUNDUP(sizeof(heap_header), ALIGN_SIZE)*/;

        if(space <= itspace){
            if (it->left){
                it = it->left;
            }
            else{
                it->left = f;
                f->parent = it;
                break;
            }
        }
        else{
            if (it->right){
                it = it->right;
            }
            else{
                it->right = f;
                f->parent = it;
                break;
            }
        }
    }

    f->left = NULL;
    f->right = NULL;
    f->header.next_color |= 1;

    /*Case 1 already handles by root and case 2*/
    for(;;){
        if (!f->parent) {
            /*Case 1*/
            f->header.next_color &= ~1;
            break;
        }
        else if(!(f->parent->header.next_color & 1)){
            /*Case 2*/
            break;
        }
        else{
            heap_free *u = freeuncle(f);
            if(u && (u->header.next_color & 1)){
                /*Case 3*/
                f->parent->header.next_color &= ~1;
                u->header.next_color &= ~1;
                f->parent->parent->header.next_color |= 1;

                f = f->parent->parent;
            }
            else{
                /*Case 4*/
                heap_free *p = f->parent;
                heap_free *gp = f->parent->parent;
                if((f == p->right) & (p == gp->left)){
                    freerotateleft(h, p);
                    f = p;
                }
                else if((f == p->left) & (p == gp->right)){
                    freerotateright(h, p);
                    f = p;
                }

                p = f->parent;
                gp = f->parent->parent;

                if(f == p->left){
                    freerotateright(h, gp);
                }
                else{
                    freerotateleft(h, gp);
                }
                p->header.next_color &= ~1;
                gp->header.next_color |= 1;

                break;
            }
        }

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
    removefree(heap, bestfree);

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
    newfree->header.prev_alloc &= ~1;
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

    
    insertfree(heap, newfree); 
    
    return;
}