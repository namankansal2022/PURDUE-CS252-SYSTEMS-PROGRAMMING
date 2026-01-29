#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "myMalloc.h"
#include "printing.h"

#ifdef TEST_ASSERT
inline static void assert(int e) {
    if (!e) {
        const char * msg = "Assertion Failed!\n";
        write(2, msg, strlen(msg));
        exit(1);
    }
}
#else
#include <assert.h>
#endif

static pthread_mutex_t mutex;
header freelistSentinels[N_LISTS];
header * lastFencePost;
void * base;
header * osChunkList [MAX_OS_CHUNKS];
size_t numOsChunks = 0;
static void init (void) __attribute__ ((constructor));
static bool isMallocInitialized;

static inline header * get_header_from_offset(void * ptr, ptrdiff_t off) {
    return (header *)((char *) ptr + off);
}

header * get_right_header(header * h) {
    return get_header_from_offset(h, get_size(h));
}

inline static header * get_left_header(header * h) {
    return get_header_from_offset(h, -h->left_size);
}

inline static void initialize_fencepost(header * fp, size_t left_size) {
    set_state(fp,FENCEPOST);
    set_size(fp, ALLOC_HEADER_SIZE);
    fp->left_size = left_size;
}

inline static void insert_os_chunk(header * hdr) {
    if (numOsChunks < MAX_OS_CHUNKS) {
        osChunkList[numOsChunks++] = hdr;
    }
}

inline static void insert_fenceposts(void * raw_mem, size_t size) {
    char * mem = (char *) raw_mem;
    header * leftFencePost = (header *) mem;
    initialize_fencepost(leftFencePost, ALLOC_HEADER_SIZE);
    header * rightFencePost = get_header_from_offset(mem, size - ALLOC_HEADER_SIZE);
    initialize_fencepost(rightFencePost, size - 2 * ALLOC_HEADER_SIZE);
}

static header * allocate_chunk(size_t size) {
    void * mem = sbrk(size);
    if (mem == (void*)-1) {
        return NULL;
    }

    insert_fenceposts(mem, size);

    header * hdr = (header *) ((char *)mem + ALLOC_HEADER_SIZE);
    set_state(hdr, UNALLOCATED);
    set_size(hdr, size - 2 * ALLOC_HEADER_SIZE);
    hdr->left_size = ALLOC_HEADER_SIZE;

    header * left_fp  = (header *) mem;
    header * right_fp = (header *) ((char *)mem + size - ALLOC_HEADER_SIZE);

    insert_os_chunk(left_fp);
    lastFencePost = right_fp;

    return hdr;
}

static const size_t MIN_DATA  = 16;
static const size_t MIN_BLOCK = ALLOC_HEADER_SIZE + MIN_DATA;

static inline int size_to_list_index(size_t sz) {
    int idx = 0;
    size_t bucket = MIN_BLOCK;
    while (idx < N_LISTS - 1 && bucket < sz) {
        idx++;
        bucket += 8;
    }
    return idx;
}

static inline void insert_into_freelist(header *blk) {
    int idx = size_to_list_index(get_size(blk));
    header *sentinel = &freelistSentinels[idx];
    blk->next = sentinel;
    blk->prev = sentinel->prev;
    sentinel->prev->next = blk;
    sentinel->prev = blk;
}

static inline void remove_from_freelist(header *blk) {
    blk->prev->next = blk->next;
    blk->next->prev = blk->prev;
}

static inline header * allocate_object(size_t raw_size) {
    if (raw_size == 0) return NULL;

    size_t aligned_size = (raw_size + 7) & ~0x7;
    if (aligned_size < MIN_DATA) aligned_size = MIN_DATA;
    size_t total_size = ALLOC_HEADER_SIZE + aligned_size;

    int list_index = size_to_list_index(total_size);

    while (1) {
        header *chosen_block = NULL;

        for (int i = list_index; i < N_LISTS; i++) {
            header *sentinel = &freelistSentinels[i];
            for (header *cur = sentinel->next; cur != sentinel; cur = cur->next) {
                if (get_state(cur) == UNALLOCATED && get_size(cur) >= total_size) {
                    chosen_block = cur;
                    break;
                }
            }
            if (chosen_block) break;
        }

        if (chosen_block) {
            remove_from_freelist(chosen_block);
            size_t remaining_size = get_size(chosen_block) - total_size;

            if (remaining_size >= MIN_BLOCK) {
                header *split_block = get_header_from_offset(chosen_block, total_size);
                set_size_and_state(split_block, remaining_size, UNALLOCATED);
                split_block->left_size = total_size;

                header *right_after_split = get_right_header(split_block);
                right_after_split->left_size = remaining_size;

                insert_into_freelist(split_block);
                set_size(chosen_block, total_size);
            } else {
                set_size(chosen_block, get_size(chosen_block));
            }

            set_state(chosen_block, ALLOCATED);
            header *right = get_right_header(chosen_block);
            right->left_size = get_size(chosen_block);
            return chosen_block;
        }


        header *prev_last = lastFencePost;
        header *new_block = allocate_chunk(ARENA_SIZE);
        if (!new_block) return NULL;

        char *new_left_fp  = ((char *)new_block) - ALLOC_HEADER_SIZE;
        header *new_right_fp = (header *)(new_left_fp + ARENA_SIZE - ALLOC_HEADER_SIZE);

        if ((char *)prev_last + ALLOC_HEADER_SIZE == new_left_fp) {
            header *left_of_prev = get_left_header(prev_last);

          if (get_state(left_of_prev) == ALLOCATED) {

    header *alloc_hdr = (header *)((char *)prev_last + ALLOC_HEADER_SIZE);
    set_size_and_state(alloc_hdr, total_size, ALLOCATED);
    alloc_hdr->left_size = get_size(left_of_prev);


             
                header *free_hdr = get_header_from_offset(alloc_hdr, total_size);
                size_t free_size = (size_t)((char *)new_right_fp - (char *)free_hdr);

                if (free_size >= MIN_BLOCK) {
                    set_size_and_state(free_hdr, free_size, UNALLOCATED);
                    free_hdr->left_size = total_size;
                    new_right_fp->left_size = free_size;
                    insert_into_freelist(free_hdr);
                } else {
                    new_right_fp->left_size = total_size;
                }

                lastFencePost = new_right_fp;
                return alloc_hdr;
            } else {
         
                remove_from_freelist(left_of_prev);
                size_t merged = (size_t)((char *)new_right_fp - (char *)left_of_prev);
                set_size_and_state(left_of_prev, merged, UNALLOCATED);
                new_right_fp->left_size = merged;
                insert_into_freelist(left_of_prev);
                lastFencePost = new_right_fp;
            }
        } else {
            insert_into_freelist(new_block);
        }
   
    }
}

static inline header * ptr_to_header(void * p) {
    return (header *)((char *) p - ALLOC_HEADER_SIZE);
}

static inline void deallocate_object(void * p) {
    if (p == NULL) return;

    header *h = ptr_to_header(p);

    if (get_state(h) != ALLOCATED) {
        fprintf(stderr, "Double Free Detected\n");
        assert(0);
    }

    set_state(h, UNALLOCATED);

    header *right = get_right_header(h);
    if (get_state(right) == UNALLOCATED) {
        remove_from_freelist(right);
        size_t new_sz = get_size(h) + get_size(right);
        set_size(h, new_sz);
        header *rr = get_right_header(h);
        rr->left_size = new_sz;
    }

    header *left = get_left_header(h);
    if (get_state(left) == UNALLOCATED) {
        remove_from_freelist(left);
        size_t new_sz = get_size(left) + get_size(h);
        set_size(left, new_sz);
        header *r = get_right_header(left);
        r->left_size = new_sz;
        h = left;
    }

    insert_into_freelist(h);
}



static inline header * detect_cycles() {
    for (int i = 0; i < N_LISTS; i++) {
        header * freelist = &freelistSentinels[i];
        for (header * slow = freelist->next, * fast = freelist->next->next;
             fast != freelist;
             slow = slow->next, fast = fast->next->next) {
            if (slow == fast) {
                return slow;
            }
        }
    }
    return NULL;
}

static inline header * verify_pointers() {
    for (int i = 0; i < N_LISTS; i++) {
        header * freelist = &freelistSentinels[i];
        for (header * cur = freelist->next; cur != freelist; cur = cur->next) {
            if (cur->next->prev != cur || cur->prev->next != cur) {
                return cur;
            }
        }
    }
    return NULL;
}

static inline bool verify_freelist() {
    header * cycle = detect_cycles();
    if (cycle != NULL) {
        fprintf(stderr, "Cycle Detected\n");
        print_sublist(print_object, cycle->next, cycle);
        return false;
    }
    header * invalid = verify_pointers();
    if (invalid != NULL) {
        fprintf(stderr, "Invalid pointers\n");
        print_object(invalid);
        return false;
    }
    return true;
}

static inline header * verify_chunk(header * chunk) {
    if (get_state(chunk) != FENCEPOST) {
        fprintf(stderr, "Invalid fencepost\n");
        print_object(chunk);
        return chunk;
    }
    for (; get_state(chunk) != FENCEPOST; chunk = get_right_header(chunk)) {
        if (get_size(chunk)  != get_right_header(chunk)->left_size) {
            fprintf(stderr, "Invalid sizes\n");
            print_object(chunk);
            return chunk;
        }
    }
    return NULL;
}

static inline bool verify_tags() {
    for (size_t i = 0; i < numOsChunks; i++) {
        header * invalid = verify_chunk(osChunkList[i]);
        if (invalid != NULL) {
            return false;
        }
    }
    return true;
}



static void init() {
    pthread_mutex_init(&mutex, NULL);

#ifdef DEBUG
    setvbuf(stdout, NULL, _IONBF, 0);
#endif

    header * block = allocate_chunk(ARENA_SIZE);
    base = ((char *) block) - ALLOC_HEADER_SIZE;

    for (int i = 0; i < N_LISTS; i++) {
        header * freelist = &freelistSentinels[i];
        freelist->next = freelist;
        freelist->prev = freelist;
    }

    header * freelist = &freelistSentinels[N_LISTS - 1];
    freelist->next = block;
    freelist->prev = block;
    block->next = freelist;
    block->prev = freelist;
}

void * my_malloc(size_t size) {
    if (size == 0) return NULL;
    pthread_mutex_lock(&mutex);
    header * hdr = allocate_object(size);
    pthread_mutex_unlock(&mutex);
    if (!hdr) return NULL;
    return hdr->data;
}

void * my_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = my_malloc(total);
    if (!p) return NULL;
    memset(p, 0, total);
    return p;
}

void * my_realloc(void * ptr, size_t size) {
    if (ptr == NULL) return my_malloc(size);
    if (size == 0) { my_free(ptr); return NULL; }
    void * mem = my_malloc(size);
    if (!mem) return NULL;
    header *h = ptr_to_header(ptr);
    size_t copy_size = get_size(h) - ALLOC_HEADER_SIZE;
    if (size < copy_size) copy_size = size;
    memcpy(mem, ptr, copy_size);
    my_free(ptr);
    return mem;
}

void my_free(void * p) {
    pthread_mutex_lock(&mutex);
    deallocate_object(p);
    pthread_mutex_unlock(&mutex);
}

bool verify() {
    return verify_freelist() && verify_tags();
}
