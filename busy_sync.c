#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <inttypes.h>

#include <drm/drm.h>      // syncobj UAPI
#include <drm/xe_drm.h>   // Xe UAPI (libdrm with xe support, or kernel uapi)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define BO_SIZE        4096
#define BIND_ADDRESS   0x1000000ull   /* arbitrary GPU VA, page aligned */

/* MI_BATCH_BUFFER_END: opcode 0x0A in bits 31:23 */
#define MI_BATCH_BUFFER_END (0x0A << 23)

static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

static int open_render_node(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }
    return fd;
}

/*
 * Query engines and return first RENDER engine instance.
 */
static struct drm_xe_engine_class_instance
pick_render_engine(int fd)
{
    struct drm_xe_device_query query = {
        .extensions = 0,
        .query      = DRM_XE_DEVICE_QUERY_ENGINES,
        .size       = 0,
        .data       = 0,
    };

    if (ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) < 0)
        die("DRM_IOCTL_XE_DEVICE_QUERY (size)");

    struct drm_xe_query_engines *engines = malloc(query.size);
    if (!engines)
        die("malloc engines");

    query.data = (uintptr_t)engines;

    if (ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) < 0)
        die("DRM_IOCTL_XE_DEVICE_QUERY (ENGINES)");

    for (uint32_t i = 0; i < engines->num_engines; i++) {
        struct drm_xe_engine *e = &engines->engines[i];
        if (e->instance.engine_class == DRM_XE_ENGINE_CLASS_RENDER) {
            struct drm_xe_engine_class_instance inst = e->instance;
            free(engines);
            return inst;
        }
    }

    free(engines);
    fprintf(stderr, "No RENDER engine found\n");
    exit(EXIT_FAILURE);
}

/*
 * Query memory regions and return a placement mask for SYSMEM.
 */
static uint32_t pick_sysmem_placement(int fd, uint32_t *min_page_size_out)
{
    struct drm_xe_device_query query = {
        .extensions = 0,
        .query      = DRM_XE_DEVICE_QUERY_MEM_REGIONS,
        .size       = 0,
        .data       = 0,
    };

    if (ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) < 0)
        die("DRM_IOCTL_XE_DEVICE_QUERY (size MEM_REGIONS)");

    struct drm_xe_query_mem_regions *mem = malloc(query.size);
    if (!mem)
        die("malloc mem_regions");

    query.data = (uintptr_t)mem;

    if (ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) < 0)
        die("DRM_IOCTL_XE_DEVICE_QUERY (MEM_REGIONS)");

    uint32_t placement = 0;
    uint32_t min_page_size = 4096;

    for (uint32_t i = 0; i < mem->num_mem_regions; i++) {
        struct drm_xe_mem_region *r = &mem->mem_regions[i];

        if (r->mem_class == DRM_XE_MEM_REGION_CLASS_SYSMEM) {
            placement |= (1u << r->instance);
            if (r->min_page_size > min_page_size)
                min_page_size = r->min_page_size;
        }
    }

    free(mem);

    if (!placement) {
        fprintf(stderr, "No SYSMEM region found; placement=0 (may fail)\n");
    }

    *min_page_size_out = min_page_size;
    return placement;
}

/* Create a binary syncobj and return its handle. */
static uint32_t create_syncobj(int fd)
{
    struct drm_syncobj_create create = {
        .handle = 0,
        .flags  = 0,  /* unsignaled binary syncobj */
    };

    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0)
        die("DRM_IOCTL_SYNCOBJ_CREATE");

    return create.handle;
}

/* Reset syncobj to unsignaled state (for reuse). */
static void reset_syncobj(int fd, uint32_t handle)
{
    struct drm_syncobj_array array = {
        .handles = (uintptr_t)&handle,
        .count_handles = 1,
        .pad = 0,
    };

    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &array) < 0)
        die("DRM_IOCTL_SYNCOBJ_RESET");
}

/* Wait for syncobj to signal (binary wait). */
static void wait_syncobj(int fd, uint32_t handle)
{
    struct drm_syncobj_wait wait = {
        .handles        = (uintptr_t)&handle,
        .timeout_nsec   = INT64_MAX,   /* "infinite" timeout */
        .count_handles  = 1,
        .flags          = 0,
        .first_signaled = 0,
        .pad            = 0,
        .deadline_nsec  = 0,
    };

    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait) < 0)
        die("DRM_IOCTL_SYNCOBJ_WAIT");
}

int main(int argc, char **argv)
{
    const char *node = "/dev/dri/renderD128";
    if (argc > 1)
        node = argv[1];

    int fd = open_render_node(node);
    if (fd < 0)
        return EXIT_FAILURE;

    printf("Opened %s\n", node);

    /* 1) Create VM */
    struct drm_xe_vm_create vmc = {
        .extensions = 0,
        .flags      = 0,  /* simple VM */
        .vm_id      = 0,
    };

    if (ioctl(fd, DRM_IOCTL_XE_VM_CREATE, &vmc) < 0)
        die("DRM_IOCTL_XE_VM_CREATE");

    uint32_t vm_id = vmc.vm_id;
    printf("VM created: id=%u\n", vm_id);

    /* 2) Pick memory placement */
    uint32_t min_page_size = 0;
    uint32_t placement = pick_sysmem_placement(fd, &min_page_size);

    if (!placement) {
        fprintf(stderr, "WARNING: placement mask is 0, GEM_CREATE may fail\n");
    }

    /* 3) Create GEM buffer attached to this VM */
    struct drm_xe_gem_create gcreate = {
        .extensions  = 0,
        .size        = BO_SIZE,
        .placement   = placement,
        .flags       = 0,
        .vm_id       = vm_id,
        .handle      = 0,
        .cpu_caching = DRM_XE_GEM_CPU_CACHING_WB,
    };

    if (ioctl(fd, DRM_IOCTL_XE_GEM_CREATE, &gcreate) < 0)
        die("DRM_IOCTL_XE_GEM_CREATE");

    uint32_t bo_handle = gcreate.handle;
    printf("GEM BO created: handle=%u, size=%" PRIu64 "\n",
           bo_handle, (uint64_t)gcreate.size);

    /* 4) mmap the BO */
    struct drm_xe_gem_mmap_offset mmo = {
        .extensions = 0,
        .handle     = bo_handle,
        .flags      = 0,
        .offset     = 0,
    };

    if (ioctl(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo) < 0)
        die("DRM_IOCTL_XE_GEM_MMAP_OFFSET");

    void *map = mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, mmo.offset);
    if (map == MAP_FAILED)
        die("mmap BO");

    printf("BO mapped at %p\n", map);

    /* 5) Write a tiny batch: just MI_BATCH_BUFFER_END */
    uint32_t *batch = (uint32_t *)map;
    batch[0] = MI_BATCH_BUFFER_END;
    batch[1] = 0;   /* padding / NOOP */
    /* msync removed: not needed and may return EINVAL */

    /* 6) Bind BO into the VM at BIND_ADDRESS (synchronous bind) */
    struct drm_xe_vm_bind bind = {
        .extensions    = 0,
        .vm_id         = vm_id,
        .exec_queue_id = 0,      /* default VM-bind engine */
        .pad           = 0,
        .num_binds     = 1,
        .num_syncs     = 0,      /* syncs only for ASYNC bind */
        .syncs         = 0,
    };

    bind.bind.extensions  = 0;
    bind.bind.obj         = bo_handle;
    bind.bind.pat_index   = 0;        /* simple PAT */
    bind.bind.obj_offset  = 0;
    bind.bind.range       = BO_SIZE;
    bind.bind.addr        = BIND_ADDRESS;
    bind.bind.op          = DRM_XE_VM_BIND_OP_MAP;
    bind.bind.flags       = 0;
    bind.bind.prefetch_mem_region_instance = 0;
    bind.bind.pad2        = 0;

    if (ioctl(fd, DRM_IOCTL_XE_VM_BIND, &bind) < 0)
        die("DRM_IOCTL_XE_VM_BIND");

    printf("BO bound at VA 0x%llx\n",
           (unsigned long long)BIND_ADDRESS);

    /* 7) Pick a RENDER engine instance */
    struct drm_xe_engine_class_instance inst = pick_render_engine(fd);

    printf("Using RENDER engine: class=%u instance=%u gt_id=%u\n",
           inst.engine_class, inst.engine_instance, inst.gt_id);

    /* 8) Create exec queue for that engine + VM */
    struct drm_xe_exec_queue_create execq = {
        .extensions     = 0,
        .width          = 1,            /* 1 slot */
        .num_placements = 1,            /* 1 engine */
        .vm_id          = vm_id,
        .flags          = 0,
        .exec_queue_id  = 0,
        .instances      = (uintptr_t)&inst,
    };

    if (ioctl(fd, DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &execq) < 0)
        die("DRM_IOCTL_XE_EXEC_QUEUE_CREATE");

    uint32_t exec_queue_id = execq.exec_queue_id;
    printf("Exec queue created: id=%u\n", exec_queue_id);

    /* 9) Create a syncobj to use as an out-fence repeatedly */
    uint32_t sync_handle = create_syncobj(fd);

    struct drm_xe_sync sync = {
        .extensions     = 0,
        .type           = DRM_XE_SYNC_TYPE_SYNCOBJ,
        .flags          = DRM_XE_SYNC_FLAG_SIGNAL,  /* signal on completion */
        .handle         = sync_handle,
        .timeline_value = 0,                        /* not used by binary */
    };

    /* Prepare EXEC struct – now with one sync */
    struct drm_xe_exec exec = {
        .extensions       = 0,
        .exec_queue_id    = exec_queue_id,
        .num_syncs        = 1,
        .syncs            = (uintptr_t)&sync,
        .address          = BIND_ADDRESS,    /* GPU VA of batch */
        .num_batch_buffer = 1,
    };

    printf("Entering infinite submit loop with syncobj.\n");
    printf("Kill this process (Ctrl+C) to stop.\n");

    /* 10) Keep submitting the same tiny batch forever */
    while (1) {
        /* Reset syncobj to unsignaled state before each submit */
        reset_syncobj(fd, sync_handle);

        /* Submit batch */
        if (ioctl(fd, DRM_IOCTL_XE_EXEC, &exec) < 0)
            die("DRM_IOCTL_XE_EXEC");

        /* Wait until GPU signals syncobj (batch completed) */
        wait_syncobj(fd, sync_handle);

        /* Optional: reduce CPU usage slightly
         * usleep(1000); // 1ms sleep
         */
    }

    /* Not reached, but for completeness */
    munmap(map, BO_SIZE);
    close(fd);

    return 0;
}
