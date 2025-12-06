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

#include <drm/drm.h>        // syncobj / GEM UAPI
#include <drm/xe_drm.h>     // Xe UAPI
#include <xf86drm.h>        // libdrm core
#include <xf86drmMode.h>    // libdrm KMS

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* MI_BATCH_BUFFER_END: opcode 0x0A in bits 31:23 */
#define MI_BATCH_BUFFER_END (0x0A << 23)

/* Arbitrary GPU VA for batch and color buffer (must be page-aligned). */
#define GPU_BATCH_ADDR  0x0000000100000000ULL
#define GPU_COLOR_ADDR  0x0000000200000000ULL

static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

/* ---------------- Xe helpers ---------------- */

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

/* ---------------- KMS helpers ---------------- */

struct kms_info {
    int fd;
    uint32_t conn_id;
    uint32_t crtc_id;
    drmModeModeInfo mode;
};

static int open_card_node(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
    }
    return fd;
}

/*
 * Find first connected connector + its CRTC + preferred mode.
 */
static struct kms_info kms_setup_basic(int fd)
{
    struct kms_info info = { .fd = fd };
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        die("drmModeGetResources");
    }

    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn)
            continue;
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            info.conn_id = conn->connector_id;
            info.mode = conn->modes[0]; /* use first mode (often preferred) */
            break;
        }
        drmModeFreeConnector(conn);
        conn = NULL;
    }

    if (!conn) {
        drmModeFreeResources(res);
        fprintf(stderr, "No connected connector found\n");
        exit(EXIT_FAILURE);
    }

    drmModeEncoder *enc = NULL;
    if (conn->encoder_id)
        enc = drmModeGetEncoder(fd, conn->encoder_id);

    if (!enc && res->count_encoders > 0) {
        enc = drmModeGetEncoder(fd, res->encoders[0]);
    }

    if (!enc) {
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        fprintf(stderr, "No encoder found\n");
        exit(EXIT_FAILURE);
    }

    info.crtc_id = enc->crtc_id;

    printf("KMS: connector=%u crtc=%u mode=%s %dx%d\n",
           info.conn_id, info.crtc_id, info.mode.name,
           info.mode.hdisplay, info.mode.vdisplay);

    drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    return info;
}

/*
 * Paint a simple gradient into the mapped BO (CPU-side).
 * Format: 32-bit XRGB8888 (we ignore alpha).
 */
static void paint_gradient(uint8_t *buf, int width, int height, int stride)
{
    for (int y = 0; y < height; y++) {
        uint32_t *row = (uint32_t *)(buf + y * stride);
        for (int x = 0; x < width; x++) {
            uint8_t r = (x * 255) / width;
            uint8_t g = (y * 255) / height;
            uint8_t b = 128;
            uint32_t pixel = (r << 16) | (g << 8) | (b);
            row[x] = pixel;
        }
    }
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    const char *render_node = "/dev/dri/renderD128";
    const char *card_node   = "/dev/dri/card0";

    if (argc > 1)
        render_node = argv[1];

    int render_fd = open_render_node(render_node);
    if (render_fd < 0)
        return EXIT_FAILURE;

    int kms_fd = open_card_node(card_node);
    if (kms_fd < 0) {
        fprintf(stderr, "KMS card node open failed; need /dev/dri/card0 (or similar) for modesetting\n");
        return EXIT_FAILURE;
    }

    printf("Opened render node: %s\n", render_node);
    printf("Opened card node  : %s\n", card_node);

    /* --- Set up KMS (connector/mode/CRTC) --- */
    struct kms_info kms = kms_setup_basic(kms_fd);

    uint32_t width  = kms.mode.hdisplay;
    uint32_t height = kms.mode.vdisplay;
    uint32_t bpp    = 32;      /* XRGB8888 */
    uint32_t cpp    = bpp / 8; /* bytes per pixel */
    uint32_t stride = width * cpp;
    uint64_t bo_size = (uint64_t)stride * height;

    printf("Using resolution %ux%u, bo_size=%" PRIu64 "\n",
           width, height, bo_size);

    /* --- 1) Create Xe VM --- */
    struct drm_xe_vm_create vmc = {
        .extensions = 0,
        .flags      = 0,  /* simple VM */
        .vm_id      = 0,
    };

    if (ioctl(render_fd, DRM_IOCTL_XE_VM_CREATE, &vmc) < 0)
        die("DRM_IOCTL_XE_VM_CREATE");

    uint32_t vm_id = vmc.vm_id;
    printf("VM created: id=%u\n", vm_id);

    /* --- 2) Pick memory placement --- */
    uint32_t min_page_size = 0;
    uint32_t placement = pick_sysmem_placement(render_fd, &min_page_size);

    if (!placement) {
        fprintf(stderr, "WARNING: placement mask is 0, GEM_CREATE may fail\n");
    }

    /* --- 3) Create GEM buffer attached to this VM (framebuffer-sized) --- */
    struct drm_xe_gem_create gcreate = {
        .extensions  = 0,
        .size        = bo_size,
        .placement   = placement,
        .flags       = 0,
        .vm_id       = vm_id,
        .handle      = 0,
        .cpu_caching = DRM_XE_GEM_CPU_CACHING_WB,
    };

    if (ioctl(render_fd, DRM_IOCTL_XE_GEM_CREATE, &gcreate) < 0)
        die("DRM_IOCTL_XE_GEM_CREATE");

    uint32_t bo_handle = gcreate.handle;
    printf("GEM BO created (render target): handle=%u, size=%" PRIu64 "\n",
           bo_handle, (uint64_t)gcreate.size);

    /* --- 4) mmap the BO on CPU side --- */
    struct drm_xe_gem_mmap_offset mmo = {
        .extensions = 0,
        .handle     = bo_handle,
        .flags      = 0,
        .offset     = 0,
    };

    if (ioctl(render_fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo) < 0)
        die("DRM_IOCTL_XE_GEM_MMAP_OFFSET");

    void *map = mmap(NULL, bo_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, render_fd, mmo.offset);
    if (map == MAP_FAILED)
        die("mmap BO");

    printf("BO mapped at %p\n", map);

    /* For now, fill it with CPU gradient (visible on screen). */
    paint_gradient((uint8_t *)map, width, height, stride);

    /* --- 5) Create a tiny batch (still just MI_BATCH_BUFFER_END) --- */

    /* Create separate batch BO (can be small, e.g., 4KB) */
    const uint64_t BATCH_BO_SIZE = 4096;
    struct drm_xe_gem_create batch_create = {
        .extensions  = 0,
        .size        = BATCH_BO_SIZE,
        .placement   = placement,
        .flags       = 0,
        .vm_id       = vm_id,
        .handle      = 0,
        .cpu_caching = DRM_XE_GEM_CPU_CACHING_WB,
    };
    if (ioctl(render_fd, DRM_IOCTL_XE_GEM_CREATE, &batch_create) < 0)
        die("DRM_IOCTL_XE_GEM_CREATE batch");

    uint32_t batch_handle = batch_create.handle;
    printf("Batch BO created: handle=%u\n", batch_handle);

    struct drm_xe_gem_mmap_offset mmo_batch = {
        .extensions = 0,
        .handle     = batch_handle,
        .flags      = 0,
        .offset     = 0,
    };
    if (ioctl(render_fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo_batch) < 0)
        die("DRM_IOCTL_XE_GEM_MMAP_OFFSET batch");

    void *batch_map = mmap(NULL, BATCH_BO_SIZE, PROT_READ | PROT_WRITE,
                           MAP_SHARED, render_fd, mmo_batch.offset);
    if (batch_map == MAP_FAILED)
        die("mmap batch BO");

    uint32_t *batch = (uint32_t *)batch_map;
    batch[0] = MI_BATCH_BUFFER_END;
    batch[1] = 0;

    /* --- 6) Bind BOs into VM (color buffer + batch) --- */

    struct drm_xe_vm_bind_op bind_ops[2];

    memset(bind_ops, 0, sizeof(bind_ops));

    /* Bind batch at GPU_BATCH_ADDR */
    bind_ops[0].extensions = 0;
    bind_ops[0].obj        = batch_handle;
    bind_ops[0].obj_offset = 0;
    bind_ops[0].addr       = GPU_BATCH_ADDR;
    bind_ops[0].range      = BATCH_BO_SIZE;
    bind_ops[0].op         = DRM_XE_VM_BIND_OP_MAP;
    bind_ops[0].flags      = 0;

    /* Bind color buffer at GPU_COLOR_ADDR (render target) */
    bind_ops[1].extensions = 0;
    bind_ops[1].obj        = bo_handle;
    bind_ops[1].obj_offset = 0;
    bind_ops[1].addr       = GPU_COLOR_ADDR;
    bind_ops[1].range      = bo_size;
    bind_ops[1].op         = DRM_XE_VM_BIND_OP_MAP;
    bind_ops[1].flags      = 0;

    struct drm_xe_vm_bind bind = {
        .extensions    = 0,
        .vm_id         = vm_id,
        .exec_queue_id = 0,      /* default VM-bind engine */
        .pad           = 0,
        .num_binds     = 2,
        .num_syncs     = 0,
        .syncs         = 0,
        .bind          = (uintptr_t)bind_ops,
    };

    if (ioctl(render_fd, DRM_IOCTL_XE_VM_BIND, &bind) < 0)
        die("DRM_IOCTL_XE_VM_BIND");

    printf("BOs bound at GPU_BATCH_ADDR=0x%llx, GPU_COLOR_ADDR=0x%llx\n",
           (unsigned long long)GPU_BATCH_ADDR,
           (unsigned long long)GPU_COLOR_ADDR);

    /* --- 7) Pick a RENDER engine instance and create exec queue --- */

    struct drm_xe_engine_class_instance inst = pick_render_engine(render_fd);

    printf("Using RENDER engine: class=%u instance=%u gt_id=%u\n",
           inst.engine_class, inst.engine_instance, inst.gt_id);

    struct drm_xe_exec_queue_create execq = {
        .extensions     = 0,
        .width          = 1,            /* 1 slot */
        .num_placements = 1,            /* 1 engine */
        .vm_id          = vm_id,
        .flags          = 0,
        .exec_queue_id  = 0,
        .instances      = (uintptr_t)&inst,
    };

    if (ioctl(render_fd, DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &execq) < 0)
        die("DRM_IOCTL_XE_EXEC_QUEUE_CREATE");

    uint32_t exec_queue_id = execq.exec_queue_id;
    printf("Exec queue created: id=%u\n", exec_queue_id);

    /* --- 8) Create a syncobj for out-fence --- */
    uint32_t sync_handle = create_syncobj(render_fd);

    struct drm_xe_sync sync = {
        .extensions     = 0,
        .type           = DRM_XE_SYNC_TYPE_SYNCOBJ,
        .flags          = DRM_XE_SYNC_FLAG_SIGNAL,  /* signal on completion */
        .handle         = sync_handle,
        .timeline_value = 0,                        /* not used by binary */
    };

    struct drm_xe_exec exec = {
        .extensions       = 0,
        .exec_queue_id    = exec_queue_id,
        .num_syncs        = 1,
        .syncs            = (uintptr_t)&sync,
        .address          = GPU_BATCH_ADDR,
        .num_batch_buffer = 1,
    };

    /* Submit once (does nothing except MI_BATCH_BUFFER_END) */
    reset_syncobj(render_fd, sync_handle);
    if (ioctl(render_fd, DRM_IOCTL_XE_EXEC, &exec) < 0)
        die("DRM_IOCTL_XE_EXEC");
    wait_syncobj(render_fd, sync_handle);
    printf("Submitted dummy batch once.\n");

    /* --- 9) Export the color BO as PRIME and import on KMS fd --- */

    struct drm_prime_handle prime = {
        .handle = bo_handle,
        .flags  = DRM_CLOEXEC | DRM_RDWR,
        .fd     = -1,
    };

    if (ioctl(render_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0)
        die("DRM_IOCTL_PRIME_HANDLE_TO_FD");

    int prime_fd = prime.fd;
    printf("Exported BO as PRIME fd=%d\n", prime_fd);

    struct drm_prime_handle prime_import = {
        .handle = 0,
        .flags  = 0,
        .fd     = prime_fd,
    };

    if (ioctl(kms_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime_import) < 0)
        die("DRM_IOCTL_PRIME_FD_TO_HANDLE");

    uint32_t kms_bo_handle = prime_import.handle;
    printf("Imported BO into KMS as handle=%u\n", kms_bo_handle);

    /* --- 10) Create framebuffer for that BO and set CRTC --- */

    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};

    handles[0] = kms_bo_handle;
    pitches[0] = stride;
    offsets[0] = 0;

    uint32_t fb_id = 0;
    int ret = drmModeAddFB2(kms_fd,
                             width, height,
                             DRM_FORMAT_XRGB8888,
                             handles, pitches, offsets,
                             &fb_id,
                             0);
    if (ret) {
        die("drmModeAddFB2");
    }

    printf("Framebuffer created: fb_id=%u\n", fb_id);

    ret = drmModeSetCrtc(kms_fd,
                         kms.crtc_id,
                         fb_id,
                         0, 0,
                         &kms.conn_id,
                         1,
                         &kms.mode);
    if (ret) {
        die("drmModeSetCrtc");
    }

    printf("Mode set; you should see the gradient full-screen now.\n");
    printf("Press Ctrl+C to exit (this example does not restore previous mode).\n");

    /* Keep running so you can see the image. */
    while (1) {
        sleep(1);
    }

    /* Not reached in this example. Normally you’d:
     * - restore old CRTC mode
     * - drmModeRmFB(kms_fd, fb_id)
     * - close prime_fd
     * - close KMS fd, destroy Xe objects, etc.
     */

    return 0;
}
