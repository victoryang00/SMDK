/*
   Copyright, Samsung Corporation

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

 * Redistributions of source code must retain the above copyright
 notice, this list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in
 the documentation and/or other materials provided with the
 distribution.

 * Neither the name of the copyright holder nor the names of its
 contributors may be used to endorse or promote products derived
 from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  */

#include "internal/init.h"
#include "internal/alloc.h"
#include "internal/config.h"
#include "jemalloc/jemalloc.h"
#include "../comp_api/include/internal/cxlmalloc.h"
#include <numaif.h>
#include <numa.h>
#include <sys/sysinfo.h>

smdk_config opt_smdk = {
    .use_exmem = true,
    .prio[0] = mem_zone_normal, /* prio high */
    .prio[1] = mem_zone_exmem, /* prio low */
    .exmem_zone_size = MAX_MEMZONE_LIMIT_MB, /* MB in size */
    .normal_zone_size = MAX_MEMZONE_LIMIT_MB, /* MB in size */
    .use_auto_arena_scaling = true, /* when enabled, create cpu*2 arenas overriding nr_normal_arena/nr_exmem_arena*/
    .nr_normal_arena = 1, /* the number of arena in arena pool */
    .nr_exmem_arena = 1, /* the number of arena in arena pool */
    .maxmemory_policy = 0, /* maxmemory_policy : oom, interleave, remain */
    .exmem_partition_range = {0, },
};

smdk_param smdk_info = {
    .current_prio = 0,
    .smdk_initialized = false,
    .get_target_arena = NULL,
};

arena_pool g_arena_pool[2];

SMDK_INLINE unsigned get_auto_scale_target_arena(mem_zone_t type){
    malloc_cpuid_t cpuid = malloc_getcpu();
    assert(cpuid >= 0);
    int pool_idx = (opt_smdk.prio[0] == type)? 0:1;
    arena_pool* pool = &g_arena_pool[pool_idx];

    return pool->arena_id[cpuid%pool->nr_arena];
}

SMDK_INLINE unsigned get_normal_target_arena(mem_zone_t type){
    int pool_idx = (opt_smdk.prio[0] == type)? 0:1;
    arena_pool* pool = &g_arena_pool[pool_idx];

    int aid = tsd_get_aid();
    if (unlikely(aid < 0)) {
        pthread_rwlock_wrlock(&pool->rwlock_arena_index);
        aid = pool->arena_index++;
        pthread_rwlock_unlock(&pool->rwlock_arena_index);
        tsd_set_aid(aid);
    }
    return pool->arena_id[aid%pool->nr_arena];
}

static int scale_arena_pool(){
    int nrcpu = sysconf(_SC_NPROCESSORS_ONLN);
    assert(nrcpu >0);

    int narenas = 0;
    if(opt_smdk.use_auto_arena_scaling){
        /* for SMP systems, create 2 normal/exmem arena per cpu by default */
        narenas = nrcpu << ARENA_AUTOSCALE_FACTOR;
        opt_smdk.nr_normal_arena = MIN(narenas, NR_ARENA_MAX);
        opt_smdk.nr_exmem_arena = MIN(narenas, NR_ARENA_MAX);
    }
    else{
        narenas = nrcpu << ARENA_SCALE_FACTOR;
        opt_smdk.nr_normal_arena = MIN(narenas, NR_ARENA_MAX);
        opt_smdk.nr_exmem_arena = MIN(narenas, NR_ARENA_MAX);
    }
    return narenas;
}

static int init_smdk_mutex(pthread_mutex_t* lock){
    if(!lock)
        return -1;

    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0) {
        return 1;
    }
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);
    if (pthread_mutex_init(lock, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        return 1;
    }
    pthread_mutexattr_destroy(&attr);
    return 0;
}

static int init_smdk_rwlock(pthread_rwlock_t* lock){
    if(!lock)
        return -1;

    pthread_rwlockattr_t attr;

    if (pthread_rwlockattr_init(&attr) != 0) {
        return 1;
    }
    pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE);
    if (pthread_rwlock_init(lock, &attr) != 0) {
        pthread_rwlockattr_destroy(&attr);
        return 1;
    }
    pthread_rwlockattr_destroy(&attr);
    return 0;
}

#define PAGE_SIZE 4096

void* node1_extent_alloc(extent_hooks_t* extent_hooks,
    void* new_addr, size_t size, size_t alignment, bool* zero,
    bool* commit, unsigned arena_index)
{
    assert(size % PAGE_SIZE == 0);
    if (new_addr)
        return NULL;
    void *mem;
    struct bitmask *bmp;

    bmp = numa_allocate_nodemask();
    numa_bitmask_setbit(bmp, 1);
    mem = opt_syscall.orig_mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                                0, 0);
    if (mem == (char *)-1)
        mem = NULL;
    else
        mbind(mem, size, MPOL_BIND, bmp ? bmp->maskp : NULL, bmp ? bmp->size + 1 : 0,
		  0);
	numa_bitmask_free(bmp);
    if(!mem)
        return NULL;
    (*zero) = false;
    (*commit) = true;
    return mem;
}

bool node1_extent_dalloc(extent_hooks_t* extent_hooks,
    void* addr, size_t size, bool committed, unsigned arena_index)
{
    assert((size_t)addr % PAGE_SIZE == 0);
    assert(size % PAGE_SIZE == 0);
    numa_free(addr, size);
    return false;
}

void node1_extent_destroy(extent_hooks_t* extent_hooks,
    void* addr, size_t size, bool committed, unsigned arena_index)
{
}

bool node1_extent_commit(extent_hooks_t* extent_hooks,
    void* addr, size_t size, size_t offset, size_t length, unsigned arena_index)
{
    return false;
}

bool node1_extent_decommit(extent_hooks_t* extent_hooks,
    void* addr, size_t size, size_t offset, size_t length, unsigned arena_index)
{
    return true;
}

bool node1_extent_purge(extent_hooks_t* extent_hooks,
    void* addr, size_t size, size_t offset, size_t length, unsigned arena_index)
{
    return true;
}

bool node1_extent_split(extent_hooks_t *extent_hooks,
    void *addr, size_t size, size_t size_a, size_t size_b, bool committed, unsigned arena_index)
{
    return false;
}

bool node1_extent_merge(extent_hooks_t* extent_hooks,
    void *addr_a, size_t size_a, void *addr_b, size_t size_b, bool committed, unsigned arena_index)
{
    return false;
}

extent_hooks_t node1_extent_hooks =
{
    .alloc = node1_extent_alloc,
    .dalloc = node1_extent_dalloc,
    .destroy = node1_extent_destroy,
    .commit = node1_extent_commit,
    .decommit = node1_extent_decommit,
    .purge_lazy = node1_extent_purge,
    .purge_forced = node1_extent_purge,
    .split = node1_extent_split,
    .merge = node1_extent_merge,
};

void* node0_extent_alloc(extent_hooks_t* extent_hooks,
    void* new_addr, size_t size, size_t alignment, bool* zero,
    bool* commit, unsigned arena_index)
{
    assert(size % PAGE_SIZE == 0);
    if (new_addr)
        return NULL;
    void *mem;
    struct bitmask *bmp;

    bmp = numa_allocate_nodemask();
    numa_bitmask_setbit(bmp, 0);
    mem = opt_syscall.orig_mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                                0, 0);
    if (mem == (char *)-1)
        mem = NULL;
    else
        mbind(mem, size, MPOL_BIND, bmp ? bmp->maskp : NULL, bmp ? bmp->size + 1 : 0,
		  0);
	numa_bitmask_free(bmp);
    if(!mem)
        return NULL;
    (*zero) = false;
    (*commit) = true;
    return mem;
}

bool node0_extent_dalloc(extent_hooks_t* extent_hooks,
    void* addr, size_t size, bool committed, unsigned arena_index)
{
    assert((size_t)addr % PAGE_SIZE == 0);
    assert(size % PAGE_SIZE == 0);
    numa_free(addr, size);
    return false;
}

void node0_extent_destroy(extent_hooks_t* extent_hooks,
    void* addr, size_t size, bool committed, unsigned arena_index)
{
}

bool node0_extent_commit(extent_hooks_t* extent_hooks,
    void* addr, size_t size, size_t offset, size_t length, unsigned arena_index)
{
    return false;
}

bool node0_extent_decommit(extent_hooks_t* extent_hooks,
    void* addr, size_t size, size_t offset, size_t length, unsigned arena_index)
{
    return true;
}

bool node0_extent_purge(extent_hooks_t* extent_hooks,
    void* addr, size_t size, size_t offset, size_t length, unsigned arena_index)
{
    return true;
}

bool node0_extent_split(extent_hooks_t *extent_hooks,
    void *addr, size_t size, size_t size_a, size_t size_b, bool committed, unsigned arena_index)
{
    return false;
}

bool node0_extent_merge(extent_hooks_t* extent_hooks,
    void *addr_a, size_t size_a, void *addr_b, size_t size_b, bool committed, unsigned arena_index)
{
    return false;
}

extent_hooks_t node0_extent_hooks =
{
    .alloc = node0_extent_alloc,
    .dalloc = node0_extent_dalloc,
    .destroy = node0_extent_destroy,
    .commit = node0_extent_commit,
    .decommit = node0_extent_decommit,
    .purge_lazy = node0_extent_purge,
    .purge_forced = node0_extent_purge,
    .split = node0_extent_split,
    .merge = node0_extent_merge,
};


static int init_arena_pool(){
    int i,j;
    size_t sz = sizeof(unsigned);
    if(opt_smdk.prio[0] == mem_zone_exmem){
        g_arena_pool[0].nr_arena = opt_smdk.nr_exmem_arena;
        g_arena_pool[0].zone_limit = opt_smdk.exmem_zone_size;

        g_arena_pool[1].nr_arena = opt_smdk.nr_normal_arena;
        g_arena_pool[1].zone_limit = opt_smdk.normal_zone_size;
    }
    else{
        g_arena_pool[0].nr_arena = opt_smdk.nr_normal_arena;
        g_arena_pool[0].zone_limit = opt_smdk.normal_zone_size;

        g_arena_pool[1].nr_arena = opt_smdk.nr_exmem_arena;
        g_arena_pool[1].zone_limit = opt_smdk.exmem_zone_size;
    }
    for(i=0;i<2;i++){
        for(j=0;j<g_arena_pool[i].nr_arena;j++){
            if(je_mallctl("arenas.create", (void *)&g_arena_pool[i].arena_id[j], &sz, NULL, 0)){
                fprintf(stderr,"arena_pool[%s] arena.create failure\n",str_priority(i));
                assert(false);
            }
            arena_t* arena = arena_get(TSDN_NULL,g_arena_pool[i].arena_id[j],false);
            assert(arena != NULL);
            if (opt_smdk.prio[i] == mem_zone_normal)
            { // prio[0] == normal, prio[1] == exmem by default
              //  normal
                char cmd[64];
                sprintf(cmd, "arena.%u.extent_hooks", g_arena_pool[i].arena_id[j]);
                extent_hooks_t *phooks = &node0_extent_hooks;
                if (je_mallctl(cmd, NULL, NULL, (void *)&phooks, sizeof(extent_hooks_t *)))
                {
                    fprintf(stderr, "je_mallctl('%s', ...) failed!\n", cmd);
                    exit(1);
                }
            }
            else
            {
                // exmem
                char cmd[64];
                sprintf(cmd, "arena.%u.extent_hooks", g_arena_pool[i].arena_id[j]);
                extent_hooks_t *phooks = &node1_extent_hooks;
                if (je_mallctl(cmd, NULL, NULL, (void *)&phooks, sizeof(extent_hooks_t *)))
                {
                    fprintf(stderr, "je_mallctl('%s', ...) failed!\n", cmd);
                    exit(1);
                }
            }
        }
        g_arena_pool[i].zone_allocated = 0;
        g_arena_pool[i].arena_index = 0;
        assert(!init_smdk_rwlock(&g_arena_pool[i].rwlock_zone_allocated));
        assert(!init_smdk_rwlock(&g_arena_pool[i].rwlock_arena_index));
        init_smdk_mutex(NULL);
    }
    return 0;
}

void init_cpu_node_config(){
    extern cpu_node_config_t je_cpu_node_config;
    je_cpu_node_config.nodemask = numa_no_nodes_ptr;
}


int init_smdk(void){
    if (smdk_info.smdk_initialized) return 0;

    init_cpu_node_config();

    if (opt_smdk.use_exmem == true) {
        scale_arena_pool();
        if(opt_smdk.use_auto_arena_scaling){
            smdk_info.get_target_arena = get_auto_scale_target_arena;
        }else{
            smdk_info.get_target_arena = get_normal_target_arena;
        }
        init_smdk_rwlock(&smdk_info.rwlock_current_prio);
        smdk_info.current_prio = 0;
        smdk_info.maxmemory_policy = opt_smdk.maxmemory_policy;

        init_arena_pool();
        smdk_info.smdk_initialized = true;
        return SMDK_RET_SUCCESS;
    } else {
        return SMDK_RET_USE_EXMEM_FALSE;
    }
}
