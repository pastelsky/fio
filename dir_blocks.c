#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <inttypes.h>

#ifdef __linux__
#include <linux/fs.h>
#include <linux/fiemap.h>
#endif

#include "fio.h"
#include "dir_blocks.h"
#include "lib/axmap.h"

struct targ_dir_info
{
    char *path;              /* Path to target directory */
    struct axmap *block_map; /* Map of blocks belonging to this directory */
    uint64_t total_blocks;   /* Total number of tracked blocks */
};

#ifdef __linux__
/*
 * Get file extent information using FIEMAP ioctl
 */
static int get_file_extents(int fd, struct fiemap **fiemap_res)
{
    struct fiemap *fiemap;
    int extents_size;
    int ret;

    /* Initial FIEMAP call to get the extent count */
    fiemap = malloc(sizeof(struct fiemap));
    if (!fiemap)
        return -ENOMEM;

    memset(fiemap, 0, sizeof(struct fiemap));
    fiemap->fm_start = 0;
    fiemap->fm_length = FIEMAP_MAX_OFFSET;
    fiemap->fm_flags = 0;
    fiemap->fm_extent_count = 0;

    ret = ioctl(fd, FS_IOC_FIEMAP, fiemap);
    if (ret < 0)
    {
        free(fiemap);
        return -errno;
    }

    /* Allocate memory for all extents */
    extents_size = sizeof(struct fiemap) +
                   fiemap->fm_mapped_extents * sizeof(struct fiemap_extent);
    fiemap = realloc(fiemap, extents_size);
    if (!fiemap)
        return -ENOMEM;

    /* Get the actual extents */
    memset(fiemap, 0, extents_size);
    fiemap->fm_start = 0;
    fiemap->fm_length = FIEMAP_MAX_OFFSET;
    fiemap->fm_flags = 0;
    fiemap->fm_extent_count = fiemap->fm_mapped_extents;

    ret = ioctl(fd, FS_IOC_FIEMAP, fiemap);
    if (ret < 0)
    {
        free(fiemap);
        return -errno;
    }

    *fiemap_res = fiemap;
    return 0;
}

/*
 * Map file extents to block numbers and mark them in the block map
 */
static int map_file_to_blocks(struct thread_data *td, const char *file_path,
                              struct targ_dir_info *dir_info)
{
    struct fiemap *fiemap = NULL;
    int fd, ret;
    unsigned int i;
    struct fio_file *f = td->files[0]; /* Use the first file for block size info */
    unsigned long long block_size = td->o.bs[DDIR_READ];

    fd = open(file_path, O_RDONLY);
    if (fd < 0)
    {
        log_err("fio: failed to open %s for block mapping: %s\n",
                file_path, strerror(errno));
        return -errno;
    }

    ret = get_file_extents(fd, &fiemap);
    close(fd);

    if (ret < 0)
    {
        log_err("fio: failed to get extents for %s: %s\n",
                file_path, strerror(-ret));
        return ret;
    }

    /* Map each extent to block numbers and mark them in the block map */
    for (i = 0; i < fiemap->fm_mapped_extents; i++)
    {
        struct fiemap_extent *extent = &fiemap->fm_extents[i];
        uint64_t block_start, block_end, block;

        /* Convert byte offsets to block numbers */
        block_start = extent->fe_physical / block_size;
        block_end = (extent->fe_physical + extent->fe_length + block_size - 1) / block_size;

        /* Mark all blocks in this extent */
        for (block = block_start; block < block_end; block++)
        {
            axmap_set(dir_info->block_map, block);
            dir_info->total_blocks++;
        }
    }

    free(fiemap);
    return 0;
}

/*
 * Recursively scan a directory and map all files to blocks
 */
static int scan_directory(struct thread_data *td, const char *path,
                          struct targ_dir_info *dir_info)
{
    DIR *dir;
    struct dirent *entry;
    int ret = 0;

    dir = opendir(path);
    if (!dir)
    {
        log_err("fio: failed to open directory %s: %s\n",
                path, strerror(errno));
        return -errno;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        char full_path[PATH_MAX];
        struct stat st;

        /* Skip . and .. */
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;

        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        if (lstat(full_path, &st) < 0)
        {
            log_err("fio: failed to stat %s: %s\n",
                    full_path, strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode))
        {
            /* Recursively process subdirectories */
            ret = scan_directory(td, full_path, dir_info);
            if (ret < 0)
                break;
        }
        else if (S_ISREG(st.st_mode))
        {
            /* Process regular files */
            ret = map_file_to_blocks(td, full_path, dir_info);
            if (ret < 0)
                break;
        }
    }

    closedir(dir);
    return ret;
}
#else
static int scan_directory(struct thread_data *td, const char *path,
                          struct targ_dir_info *dir_info)
{
    log_err("fio: --targ-dir is only supported on Linux\n");
    return -ENOSYS;
}
#endif

int init_targ_dir_blocks(struct thread_data *td)
{
    struct targ_dir_info *dir_info;
    int ret;

    if (!td->o.targ_dir)
        return 0; /* No target directory specified */

    dir_info = calloc(1, sizeof(*dir_info));
    if (!dir_info)
        return -ENOMEM;

    dir_info->path = strdup(td->o.targ_dir);
    if (!dir_info->path)
    {
        free(dir_info);
        return -ENOMEM;
    }

    /* Estimate maximum number of blocks */
    struct fio_file *f = td->files[0]; /* Use the first file for size info */
    uint64_t max_blocks = f->real_file_size / td->o.bs[DDIR_READ];

    dir_info->block_map = axmap_new(max_blocks);
    if (!dir_info->block_map)
    {
        free(dir_info->path);
        free(dir_info);
        log_err("fio: failed to allocate memory for target directory block map\n");
        return -ENOMEM;
    }

    /* Scan directory and map files to blocks */
    ret = scan_directory(td, dir_info->path, dir_info);
    if (ret < 0)
    {
        cleanup_targ_dir_blocks(td);
        return ret;
    }

    td->dir_blocks_info = dir_info;
    log_info("fio: mapped %" PRIu64 " blocks for target directory %s\n",
             dir_info->total_blocks, dir_info->path);

    return 0;
}

bool is_block_in_targ_dir(struct thread_data *td, struct fio_file *f, uint64_t block)
{
    struct targ_dir_info *dir_info;

    if (!td->dir_blocks_info)
        return true; /* No target directory specified, allow all blocks */

    dir_info = td->dir_blocks_info;
    return axmap_isset(dir_info->block_map, block);
}

void cleanup_targ_dir_blocks(struct thread_data *td)
{
    struct targ_dir_info *dir_info = td->dir_blocks_info;

    if (!dir_info)
        return;

    if (dir_info->block_map)
        axmap_free(dir_info->block_map);

    free(dir_info->path);
    free(dir_info);
    td->dir_blocks_info = NULL;
}