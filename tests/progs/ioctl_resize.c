/*
 * ioctl_resize.c --- issue EXT4_IOC_RESIZE_FS on a mount point
 *
 * ioctl_resize <mountpoint> <new_blocks> [uid]
 * ioctl_resize -s <mountpoint>
 *
 * Prints "ok" or the errno name, for the fuse4fs online resize tests, which
 * need to call the ioctl as another user and with sizes resize2fs refuses
 * to ask for; -s issues EXT4_IOC_SHUTDOWN instead.
 *
 * This file may be redistributed under the terms of the GNU Public License.
 */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "ext2fs/ext2_fs.h"

#ifndef EXT4_IOC_SHUTDOWN
#define EXT4_IOC_SHUTDOWN _IOR('X', 125, __u32)
#endif

int main(int argc, char **argv)
{
	unsigned long long blocks;
	__u32 shutdown_flags = 0;
	int fd;

	if (argc == 3 && strcmp(argv[1], "-s") == 0) {
		fd = open(argv[2], O_RDONLY);
		if (fd < 0) {
			perror(argv[2]);
			return 2;
		}
		if (ioctl(fd, EXT4_IOC_SHUTDOWN, &shutdown_flags)) {
			perror("EXT4_IOC_SHUTDOWN");
			return 1;
		}
		printf("ok\n");
		return 0;
	}
	if (argc < 3 || argc > 4) {
		fprintf(stderr, "usage: %s mountpoint new_blocks [uid]\n",
			argv[0]);
		return 2;
	}
	blocks = strtoull(argv[2], NULL, 0);
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror(argv[1]);
		return 2;
	}
	if (argc == 4) {
		uid_t uid = strtoul(argv[3], NULL, 0);

		if (setgid(uid) || setuid(uid)) {
			perror("setuid");
			return 2;
		}
	}
	if (ioctl(fd, EXT4_IOC_RESIZE_FS, &blocks) == 0) {
		printf("ok\n");
		return 0;
	}
	switch (errno) {
	case EPERM:		printf("EPERM\n"); break;
	case EINVAL:		printf("EINVAL\n"); break;
	case EOPNOTSUPP:	printf("EOPNOTSUPP\n"); break;
	case EBUSY:		printf("EBUSY\n"); break;
	case EAGAIN:		printf("EAGAIN\n"); break;
	case EROFS:		printf("EROFS\n"); break;
	case ENOTTY:		printf("ENOTTY\n"); break;
	default:		printf("errno %d\n", errno); break;
	}
	return 1;
}
