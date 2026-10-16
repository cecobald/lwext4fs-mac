/*
 * ops.h - FUSE 2 high-level operations backed by lwext4.
 *
 * Feasibility scaffold only. See ops.c for design notes and limitations.
 */
#ifndef LWE4FS_OPS_H
#define LWE4FS_OPS_H

#include <fuse.h>

/* The single fuse_operations table exported by ops.c. */
extern struct fuse_operations lwext4fs_ops;

/*
 * SAFE MODE switch. 0 (default) = read-only, 1 = read-write.
 * Defined in main.c, set from the FUSE args ("-o rw") before fuse_main().
 * ops.c enforces it at the FUSE layer: every mutating operation returns
 * -EROFS while it is 0. lwext4 itself is always mounted read-write internally
 * (see main.c) so that journal replay/recovery works; the read-only policy is
 * enforced only at this front-end.
 */
extern int g_read_write;

/*
 * Register the ops layer's recursive pthread mutex as lwext4's mountpoint OS
 * lock (struct ext4_lock) so that lwext4's internal critical sections are
 * serialised. Must be called after ext4_mount() has registered the mountpoint
 * (ext4_mount_setup_locks() looks the mountpoint up by name and returns ENOENT
 * otherwise) and before any concurrent filesystem activity.
 * mount_point must be a lwext4-style path ending in '/' (we use "/").
 */
int lwext4fs_setup_locks(const char *mount_point);

/*
 * Called by main() right after ext4_mount() succeeds, so the ops layer can
 * grab a superblock handle (needed for size/link-count extraction).
 * mount_point must be a lwext4-style path ending in '/' (we use "/").
 */
void lwext4fs_ops_init(const char *mount_point);

#endif /* LWE4FS_OPS_H */
