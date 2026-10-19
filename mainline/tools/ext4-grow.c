/*
 * ext4-grow MOUNTPOINT NEW_SIZE_IN_BLOCKS
 *
 * Online-grow a mounted ext4 with EXT4_IOC_RESIZE_FS on the mount point, the
 * same ioctl resize2fs uses. Our kernel has CONFIG_BLK_DEV_WRITE_MOUNTED off,
 * so resize2fs' read-write open of the mounted device fails with EBUSY
 * (EVIDENCE 25); the ioctl needs no open of the device.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <unistd.h>
#include <linux/types.h>

#define EXT4_IOC_RESIZE_FS	_IOW('f', 16, __u64)

int main(int argc, char **argv)
{
	struct statfs sf;
	__u64 blocks;
	char *end;
	int fd;

	if (argc != 3) {
		fprintf(stderr, "usage: ext4-grow MOUNTPOINT NEW_SIZE_IN_BLOCKS\n");
		return 2;
	}
	blocks = strtoull(argv[2], &end, 10);
	if (*end || !blocks) {
		fprintf(stderr, "bad block count '%s'\n", argv[2]);
		return 2;
	}
	if (statfs(argv[1], &sf)) {
		perror("statfs");
		return 1;
	}
	printf("%s: block size %ld, now %llu blocks, growing to %llu\n", argv[1],
	       (long)sf.f_bsize, (unsigned long long)sf.f_blocks,
	       (unsigned long long)blocks);
	if (blocks <= sf.f_blocks) {
		fprintf(stderr, "not larger than the current size, nothing to do\n");
		return 1;
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open mountpoint");
		return 1;
	}
	if (ioctl(fd, EXT4_IOC_RESIZE_FS, &blocks)) {
		fprintf(stderr, "EXT4_IOC_RESIZE_FS: %s\n", strerror(errno));
		return 1;
	}
	close(fd);
	statfs(argv[1], &sf);
	printf("done: %llu blocks\n", (unsigned long long)sf.f_blocks);
	return 0;
}
