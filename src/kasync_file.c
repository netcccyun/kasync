#include "kasync_file.h"
#include "klog.h"
#include "katom.h"
#include "kmalloc.h"
#ifdef LINUX
#include <mntent.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#ifdef LINUX_EPOLL
#include "kasync_worker.h"
/* Threads doing buffered reads that missed the page cache. */
#define KASYNC_BUFFERED_READ_WORKER 16
kasync_worker* kasync_buffered_read_worker = NULL;

#ifndef RWF_NOWAIT
#define RWF_NOWAIT 0x00000008
#endif
#ifndef __NR_preadv2
#if defined(__x86_64__) && !defined(__ILP32__)
#define __NR_preadv2 327
#elif defined(__i386__)
#define __NR_preadv2 378
#elif defined(__aarch64__)
#define __NR_preadv2 286
#endif
#endif
/* 0 unknown, 1 supported, -1 not supported by this kernel or file system */
static volatile int kasync_nowait_read_state = 0;

int kasync_file_read_cached(kasync_file* fp, char* buf, int length)
{
#ifdef __NR_preadv2
	if (kasync_nowait_read_state < 0 || fp->st.direct_io) {
		return -1;
	}
	struct iovec iov;
	iov.iov_base = buf;
	iov.iov_len = (size_t)length;
	uint64_t offset = (uint64_t)fp->st.offset;
	unsigned long pos_l = (unsigned long)offset;
	unsigned long pos_h = (unsigned long)((offset >> (sizeof(long) * 4)) >> (sizeof(long) * 4));
	long ret = syscall(__NR_preadv2, fp->st.fd, &iov, 1, pos_l, pos_h, RWF_NOWAIT);
	if (ret >= 0 || errno == EAGAIN) {
		/* EAGAIN means the flag works but the data is not cached */
		if (kasync_nowait_read_state == 0) {
			kasync_nowait_read_state = 1;
		}
		return ret >= 0 ? (int)ret : -1;
	}
	if (kasync_nowait_read_state == 0 && (errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL)) {
		/* old kernel (CentOS 7 is 3.10) or no RWF_NOWAIT on this file system */
		kasync_nowait_read_state = -1;
	}
#endif
	return -1;
}
bool kasync_file_nowait_read_supported()
{
	return kasync_nowait_read_state > 0;
}
#endif

#ifdef KF_ASYNC_WORKER
#include "kasync_worker.h"
#define KFIBER_AIO_DEFAULT_WORKER 16
static kgl_list kasync_worker_file_aio_list;
static kasync_worker* kasync_file_aio_worker = NULL;

kev_result kasync_file_worker_callback(void* data, int msec) {
	kasync_file* file = (kasync_file*)data;
	int op ;
#ifdef _WIN32
	DWORD ret;
#else
	int ret;
#endif
	switch (file->kiocb.cmd) {
	case kf_aio_read:
		op = OP_READ;
#ifdef _WIN32
		{
			HANDLE h = kasync_file_get_handle(file);
			LARGE_INTEGER li;
			li.QuadPart = _kasync_file_get_adjust_offset(file);
			if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN) ||
				!ReadFile(h, file->kiocb.buf, file->kiocb.length, &ret, NULL)) {
				ret = -1;
			}
		}
#else
		ret = (int)pread(kasync_file_get_handle(file), file->kiocb.buf, (size_t)file->kiocb.length, _kasync_file_get_adjust_offset(file));
#endif
		break;
	case kf_aio_write:
		op = OP_WRITE;
#ifdef _WIN32
		{
			HANDLE h = kasync_file_get_handle(file);
			LARGE_INTEGER li;
			li.QuadPart = _kasync_file_get_adjust_offset(file);
			if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN) ||
				!WriteFile(h, file->kiocb.buf, file->kiocb.length, &ret, NULL)) {
				ret = -1;
			}
		}
#else
		ret = (int)pwrite(kasync_file_get_handle(file), file->kiocb.buf, (size_t)file->kiocb.length, _kasync_file_get_adjust_offset(file));
#endif
		break;
	default:
		ret = -1;
		op = OP_READ;
		assert(false);
		break;
	}
	kgl_selector_module.next(file->st.base.selector, kasync_file_get_opaque(file), file->st.e[op].result, file->st.e[op].arg, kasync_file_adjust_result(file,(int)ret));
	return kev_ok;
}

bool kasync_file_worker_start(kasync_file *file)
{
	assert(kasync_file_aio_worker);
	kasync_worker_start(kasync_file_aio_worker, file, kasync_file_worker_callback);
	return true;
}
void kasync_file_worker_init() {
	klist_init(&kasync_worker_file_aio_list);
	kasync_file_aio_worker = kasync_worker_init(KFIBER_AIO_DEFAULT_WORKER, 0);
}
#endif
void init_aio_align_size()
{
	kgl_aio_align_size = 512;
#ifdef LINUX
#ifdef ANDROID
		//android
#else
	FILE *mntfile = setmntent("/proc/mounts", "r");
	if (mntfile == NULL) {
		kgl_aio_align_size = 4096;
		klog(KLOG_ERR, "open /proc/mounts file failed. now set kgl_aio_align_size=[%d]\n", kgl_aio_align_size);
		return;
	}
	struct mntent *mntent;
	while (NULL != (mntent = getmntent(mntfile))) {
		int fd = open(mntent->mnt_fsname, O_RDONLY);
		if (fd < 0) {
			continue;
		}
		int lbs = 0;
		if (0 == ioctl(fd, BLKSSZGET, &lbs)) {
			if (lbs > kgl_aio_align_size) {
				kgl_aio_align_size = lbs;
			}
		}
		close(fd);
	}
	endmntent(mntfile);
#endif
#endif
#ifdef KF_ASYNC_WORKER
	kasync_file_worker_init();
#endif
#ifdef LINUX_EPOLL
	if (kasync_buffered_read_worker == NULL) {
		kasync_buffered_read_worker = kasync_worker_init(KASYNC_BUFFERED_READ_WORKER, 0);
	}
#endif
	klog(KLOG_ERR, "kgl_aio_align_size=[%d]\n", kgl_aio_align_size);
}
void *aio_alloc_buffer(size_t size)
{
#ifdef _WIN32
	return xmalloc(size);
#else
	size = kgl_align(size, kgl_aio_align_size);
	return kgl_memalign(kgl_aio_align_size, size);
#endif
}
void aio_free_buffer(void *buf)
{
#ifdef _WIN32
	xfree(buf);
#else
	kgl_align_free(buf);
#endif
}
#ifdef LINUX_EPOLL
bool kasync_file_direct(kasync_file *fp, bool on_flag) {
	int flags = fcntl(fp->st.fd, F_GETFL);
    if (flags == -1) {
        return false;
	}
	if (fcntl(fp->st.fd, F_SETFL, on_flag?flags | O_DIRECT:flags & ~O_DIRECT) == 0) {
		fp->st.direct_io = on_flag;
		return true;
	}
	return false;
}
#endif
