#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <inttypes.h>

#include <drm/xe_drm.h>

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

static const char *mem_class_name(uint16_t cls)
{
    switch (cls) {
    case DRM_XE_MEM_REGION_CLASS_SYSMEM: return "SYSMEM";
    case DRM_XE_MEM_REGION_CLASS_VRAM:   return "VRAM";
    default:                             return "UNKNOWN";
    }
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

    /* Query size of MEM_REGIONS */
    struct drm_xe_device_query query = {
        .query = DRM_XE_DEVICE_QUERY_MEM_REGIONS,
        .size  = 0,
        .data  = 0,
    };

    if (ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) < 0)
        die("DRM_IOCTL_XE_DEVICE_QUERY (size)");

    if (query.size == 0) {
        fprintf(stderr, "Driver returned size=0 for MEM_REGIONS\n");
        return EXIT_FAILURE;
    }

    struct drm_xe_query_mem_regions *mem = malloc(query.size);
    if (!mem)
        die("malloc");

    memset(mem, 0, query.size);
    query.data = (uintptr_t)mem;

    if (ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) < 0)
        die("DRM_IOCTL_XE_DEVICE_QUERY");

    printf("num_mem_regions = %u\n\n", mem->num_mem_regions);

    uint32_t sysmem_min = 0;

    for (uint32_t i = 0; i < mem->num_mem_regions; i++) {
        struct drm_xe_mem_region *r = &mem->mem_regions[i];

        printf("Region %u:\n", i);
        printf("  class         = %s (%u)\n",
               mem_class_name(r->mem_class), r->mem_class);
        printf("  instance      = %u\n", r->instance);
        printf("  min_page_size = %" PRIu64 "\n", (uint64_t)r->min_page_size);
        printf("  total_size    = %" PRIu64 "\n\n", (uint64_t)r->total_size);

        if (r->mem_class == DRM_XE_MEM_REGION_CLASS_SYSMEM) {
            if (r->min_page_size > sysmem_min)
                sysmem_min = (uint32_t)r->min_page_size;
        }
    }

    if (sysmem_min > 0) {
        printf("== Effective SYSMEM min_page_size = %u bytes ==\n", sysmem_min);

        if (sysmem_min == 4096)
            printf("OK: 4K alignment is fine here.\n");
        else
            printf("LARGE PAGE SIZE (e.g., 64K). 4K BO / bind.range will FAIL.\n");
    } else {
        printf("No SYSMEM region reported.\n");
    }

    free(mem);
    close(fd);
    return 0;
}
