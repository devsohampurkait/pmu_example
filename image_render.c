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

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <drm/xe_drm.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

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

/* ========================== KMS HELPERS (DISPLAY) ======================== */

struct kms_state {
    int fd;

    uint32_t conn_id;
    uint32_t crtc_id;
    drmModeModeInfo mode;

    uint32_t fb_id;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
    void *map;
};

static void kms_cleanup(struct kms_state *kms)
{
    if (!kms)
        return;

    if (kms->fb_id)
        drmModeRmFB(kms->fd, kms->fb_id);

    if (kms->map && kms->size)
        munmap(kms->map, kms->size);

    if (kms->handle) {
        struct drm_mode_destroy_dumb dreq = {0};
        dreq.handle = kms->handle;
        if (ioctl(kms->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq) < 0)
            perror("DRM_IOCTL_MODE_DESTROY_DUMB");
    }

    if (kms->fd >= 0)
        close(kms->fd);
}

static int kms_init(struct kms_state *kms, const char *card_path)
{
    memset(kms, 0, sizeof(*kms));
    kms->fd = -1;

    kms->fd = open(card_path, O_RDWR | O_CLOEXEC);
    if (kms->fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", card_path, strerror(errno));
        return -1;
    }

    drmModeRes *res = drmModeGetResources(kms->fd);
    if (!res) {
        perror("drmModeGetResources");
        goto err;
    }

    drmModeConnector *conn = NULL;
    drmModeEncoder *enc = NULL;

    /* Pick first connected connector with at least one mode */
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(kms->fd, res->connectors[i]);
        if (!conn)
            continue;

        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            kms->conn_id = conn->connector_id;
            kms->mode = conn->modes[0]; /* preferred mode */
            break;
        }

        drmModeFreeConnector(conn);
        conn = NULL;
    }

    if (!conn) {
        fprintf(stderr, "No connected connector found\n");
        drmModeFreeResources(res);
        goto err;
    }

    /* Pick a CRTC */
    if (conn->encoder_id)
        enc = drmModeGetEncoder(kms->fd, conn->encoder_id);

    if (enc) {
        kms->crtc_id = enc->crtc_id;
    } else if (res->count_crtcs > 0) {
        kms->crtc_id = res->crtcs[0];
    } else {
        fprintf(stderr, "No usable CRTC found\n");
        drmModeFreeEncoder(enc);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        goto err;
    }

    drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    /* Create dumb buffer */
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width  = kms->mode.hdisplay;
    creq.height = kms->mode.vdisplay;
    creq.bpp    = 32;

    if (ioctl(kms->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        goto err;
    }

    kms->handle = creq.handle;
    kms->pitch  = creq.pitch;
    kms->size   = creq.size;

    /* Map dumb buffer */
    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = kms->handle;

    if (ioctl(kms->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        goto err;
    }

    kms->map = mmap(0, kms->size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, kms->fd, mreq.offset);
    if (kms->map == MAP_FAILED) {
        perror("mmap dumb buffer");
        kms->map = NULL;
        goto err;
    }

    /* Simple gradient fill (CPU) */
    uint32_t *pixels = (uint32_t *)kms->map;
    for (uint32_t y = 0; y < kms->mode.vdisplay; y++) {
        for (uint32_t x = 0; x < kms->mode.hdisplay; x++) {
            uint8_t r = (x * 255) / (kms->mode.hdisplay ? kms->mode.hdisplay : 1);
            uint8_t g = (y * 255) / (kms->mode.vdisplay ? kms->mode.vdisplay : 1);
            uint8_t b = 128;
            pixels[y * (kms->pitch / 4) + x] =
                (0xFFu << 24) | (r << 16) | (g << 8) | b; /* ARGB8888 */
        }
    }

    /* Create framebuffer (non-AddFB2, no DRM_FORMAT_XRGB8888) */
    if (drmModeAddFB(kms->fd,
                     kms->mode.hdisplay,
                     kms->mode.vdisplay,
                     24,              /* depth */
                     32,              /* bpp */
                     kms->pitch,
                     kms->handle,
                     &kms->fb_id)) {
        perror("drmModeAddFB");
        goto err;
    }

    if (drmModeSetCrtc(kms->fd,
                       kms->crtc_id,
                       kms->fb_id,
                       0, 0,
                       &kms->conn_id,
                       1,
                       &kms->mode)) {
        perror("drmModeSetCrtc");
        goto err;
    }

    printf("KMS: Gradient framebuffer is now displayed.\n");
    return 0;

err:
    kms_cleanup(kms);
    return -1;
}

/* ========================== XE HELPERS (RENDER) ========================= */

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
    struct drm_xe_device_query query;
    memset(&query, 0, sizeof(query));
    query.query = DRM_XE_DEVICE_QUERY_ENGINES;

    if (ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) < 0)
        die("DRM_IOCTL_XE_DEVICE_QUERY (size)");

    struct drm_xe_query_engines *engines = malloc(query.size);
    if (!engines)
        die("malloc engines");

    memset(engines, 0, query.size);
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
    struct drm_xe_device_query query;
    memset(&query, 0, sizeof(query));
    query.query = DRM_XE_DEVICE_QUERY_MEM_REGIONS;

    if (ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) < 0)
        die("DRM_IOCTL_XE_DEVICE_QUERY (size MEM_REGIONS)");

    struct drm_xe_query_mem_regions *mem = malloc(query.size);
    if (!mem)
        die("malloc mem_regions");

    memset(mem, 0, query.size);
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
    struct drm_syncobj_create create;
    memset(&create, 0, sizeof(create));

    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0)
        die("DRM_IOCTL_SYNCOBJ_CREATE");

    return create.handle;
}

/* Reset syncobj to unsignaled state (for reuse). */
static void reset_syncobj(int fd, uint32_t handle)
{
    struct drm_syncobj_array array;
    memset(&array, 0, sizeof(array));
    array.handles = (uintptr_t)&handle;
    array.count_handles = 1;

    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &array) < 0)
        die("DRM_IOCTL_SYNCOBJ_RESET");
}

/* Wait for syncobj to signal (binary wait). */
static void wait_syncobj(int fd, uint32_t handle)
{
    struct drm_syncobj_wait wait;
    memset(&wait, 0, sizeof(wait));

    wait.handles        = (uintptr_t)&handle;
    wait.timeout_nsec   = INT64_MAX;   /* "infinite" timeout */
    wait.count_handles  = 1;
    wait.flags          = 0;
    wait.first_signaled = 0;

    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait) < 0)
        die("DRM_IOCTL_SYNCOBJ_WAIT");
}

static void run_xe_noop(int fd)
{
    /* 1) Create VM */
    struct drm_xe_vm_create vmc;
    memset(&vmc, 0, sizeof(vmc));
    /* vmc.flags = 0; simple VM */

    if (ioctl(fd, DRM_IOCTL_XE_VM_CREATE, &vmc) < 0)
        die("DRM_IOCTL_XE_VM_CREATE");

    uint32_t vm_id = vmc.vm_id;
    printf("Xe: VM created: id=%u\n", vm_id);

    /* 2) Pick memory placement */
    uint32_t min_page_size = 0;
    uint32_t placement = pick_sysmem_placement(fd, &min_page_size);

    if (!placement) {
        fprintf(stderr, "Xe: WARNING: placement mask is 0, GEM_CREATE may fail\n");
    }

    /* 3) Create GEM buffer attached to this VM */
    struct drm_xe_gem_create gcreate;
    memset(&gcreate, 0, sizeof(gcreate));
    gcreate.size      = BO_SIZE;
    gcreate.placement = placement;
    gcreate.vm_id     = vm_id;
    /* flags, cpu_caching left as 0 / default */

    if (ioctl(fd, DRM_IOCTL_XE_GEM_CREATE, &gcreate) < 0)
        die("DRM_IOCTL_XE_GEM_CREATE");

    uint32_t bo_handle = gcreate.handle;
    printf("Xe: GEM BO created: handle=%u, size=%" PRIu64 "\n",
           bo_handle, (uint64_t)gcreate.size);

    /* 4) mmap the BO */
    struct drm_xe_gem_mmap_offset mmo;
    memset(&mmo, 0, sizeof(mmo));
    mmo.handle = bo_handle;

    if (ioctl(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo) < 0)
        die("DRM_IOCTL_XE_GEM_MMAP_OFFSET");

    void *map = mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, mmo.offset);
    if (map == MAP_FAILED)
        die("mmap BO");

    printf("Xe: BO mapped at %p\n", map);

    /* 5) Write a tiny batch: just MI_BATCH_BUFFER_END */
    uint32_t *batch = (uint32_t *)map;
    batch[0] = MI_BATCH_BUFFER_END;
    batch[1] = 0;   /* padding / NOOP */

    /* 6) Bind BO into the VM at BIND_ADDRESS (synchronous bind) */
    struct drm_xe_vm_bind_op bop;
    memset(&bop, 0, sizeof(bop));
    bop.obj        = bo_handle;
    bop.obj_offset = 0;
    bop.range      = BO_SIZE;
    bop.addr       = BIND_ADDRESS;
    bop.tile_mask  = 0;
    bop.op         = XE_VM_BIND_OP_MAP;   /* MAP operation */
    bop.region     = 0;

    struct drm_xe_vm_bind bind;
    memset(&bind, 0, sizeof(bind));
    bind.vm_id         = vm_id;
    bind.exec_queue_id = 0;   /* default VM bind engine */
    bind.num_binds     = 1;
    bind.bind          = bop; /* single bind op */
    /* bind.flags = 0;     // synchronous bind (default) */
    bind.num_syncs     = 0;
    bind.syncs         = 0;

    if (ioctl(fd, DRM_IOCTL_XE_VM_BIND, &bind) < 0)
        die("DRM_IOCTL_XE_VM_BIND");

    printf("Xe: BO bound at VA 0x%llx\n",
           (unsigned long long)BIND_ADDRESS);

    /* 7) Pick a RENDER engine instance */
    struct drm_xe_engine_class_instance inst = pick_render_engine(fd);

    printf("Xe: Using RENDER engine: class=%u instance=%u gt_id=%u\n",
           inst.engine_class, inst.engine_instance, inst.gt_id);

    /* 8) Create exec queue for that engine + VM */
    struct drm_xe_exec_queue_create execq;
    memset(&execq, 0, sizeof(execq));
    execq.vm_id          = vm_id;
    execq.num_bb_per_exec = 1;
    execq.num_eng_per_bb  = 1;
    execq.instances       = (uintptr_t)&inst;

    if (ioctl(fd, DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &execq) < 0)
        die("DRM_IOCTL_XE_EXEC_QUEUE_CREATE");

    uint32_t exec_queue_id = execq.exec_queue_id;
    printf("Xe: Exec queue created: id=%u\n", exec_queue_id);

    /* 9) Create a syncobj to use as an out-fence */
    uint32_t sync_handle = create_syncobj(fd);

    struct drm_xe_sync sync;
    memset(&sync, 0, sizeof(sync));
    sync.type           = DRM_XE_SYNC_TYPE_SYNCOBJ;
    sync.flags          = DRM_XE_SYNC_FLAG_SIGNAL;  /* signal on completion */
    sync.handle         = sync_handle;
    sync.timeline_value = 0;

    struct drm_xe_exec exec;
    memset(&exec, 0, sizeof(exec));
    exec.exec_queue_id    = exec_queue_id;
    exec.num_syncs        = 1;
    exec.syncs            = (uintptr_t)&sync;
    exec.address          = BIND_ADDRESS;    /* GPU VA of batch */
    exec.num_batch_buffer = 1;

    /* 10) Submit once, wait for completion */
    reset_syncobj(fd, sync_handle);

    if (ioctl(fd, DRM_IOCTL_XE_EXEC, &exec) < 0)
        die("DRM_IOCTL_XE_EXEC");

    wait_syncobj(fd, sync_handle);
    printf("Xe: Batch executed successfully.\n");

    munmap(map, BO_SIZE);
    /* For brevity we skip explicit destroy of queue/vm/bo/syncobj.
     * Kernel will clean up on fd close.
     */
}

/* ================================ main ==================================== */

int main(int argc, char **argv)
{
    const char *card_node   = "/dev/dri/card0";        /* display node */
    const char *render_node = "/dev/dri/renderD128";   /* Xe render node */

    if (argc > 1)
        card_node = argv[1];
    if (argc > 2)
        render_node = argv[2];

    struct kms_state kms;

    if (kms_init(&kms, card_node) < 0) {
        fprintf(stderr, "Failed to initialize KMS on %s\n", card_node);
        return EXIT_FAILURE;
    }

    printf("Display is set up, gradient is on screen.\n");

    int xe_fd = open_render_node(render_node);
    if (xe_fd >= 0) {
        printf("Opened %s for Xe rendering\n", render_node);
        run_xe_noop(xe_fd);
        close(xe_fd);
    } else {
        fprintf(stderr, "Xe render node open failed, skipping Xe exec.\n");
    }

    printf("Sleeping for 10 seconds so you can see the image...\n");
    sleep(10);

    kms_cleanup(&kms);
    return 0;
}
