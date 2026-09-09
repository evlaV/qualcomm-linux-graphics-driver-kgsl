#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <unistd.h>
#include <linux/sync_file.h>

int sync_wait(int fd, int timeout)
{
	struct pollfd fds = {0};
	int ret;

	fds.fd = fd;
	fds.events = POLLIN;

	do {
		ret = poll(&fds, 1, timeout);
		if (ret > 0) {
			if (fds.revents & (POLLERR | POLLNVAL)) {
				errno = EINVAL;
				return -1;
			}
			return 0;
		} else if (ret == 0) {
			errno = ETIME;
			return -1;
		}
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));

	return ret;
}

int sync_merge(const char *name, int fd1, int fd2)
{
	struct sync_merge_data data = {0};
	int ret;

	data.fd2 = fd2;
	strncpy(data.name, name, sizeof(data.name));

	do {
		ret = ioctl(fd1, SYNC_IOC_MERGE, &data);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));

	if (ret < 0)
		return ret;

	return data.fence;
}
