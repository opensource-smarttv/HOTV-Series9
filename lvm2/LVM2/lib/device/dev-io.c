/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "lvm-types.h"
#include "device.h"
#include "metadata.h"
#include "lvmcache.h"
#include "memlock.h"
#include "locking.h"

#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef linux
#  define u64 uint64_t		/* Missing without __KERNEL__ */
#  undef WNOHANG		/* Avoid redefinition */
#  undef WUNTRACED		/* Avoid redefinition */
#  include <linux/fs.h>		/* For block ioctl definitions */
#  define BLKSIZE_SHIFT SECTOR_SHIFT
#  ifndef BLKGETSIZE64		/* fs.h out-of-date */
#    define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#  endif /* BLKGETSIZE64 */
#  ifndef BLKDISCARD
#    define BLKDISCARD	_IO(0x12,119)
#  endif
#else
#  include <sys/disk.h>
#  define BLKBSZGET DKIOCGETBLOCKSIZE
#  define BLKSSZGET DKIOCGETBLOCKSIZE
#  define BLKGETSIZE64 DKIOCGETBLOCKCOUNT
#  define BLKFLSBUF DKIOCSYNCHRONIZECACHE
#  define BLKSIZE_SHIFT 0
#endif

#ifdef O_DIRECT_SUPPORT
#  ifndef O_DIRECT
#    error O_DIRECT support configured but O_DIRECT definition not found in headers
#  endif
#endif

static DM_LIST_INIT(_open_devices);

/*-----------------------------------------------------------------
 * The standard io loop that keeps submitting an io until it's
 * all gone.
 *---------------------------------------------------------------*/
static int _io(struct device_area *where, char *buffer, int should_write)
{
	int fd = dev_fd(where->dev);
	ssize_t n = 0;
	size_t total = 0;

	if (fd < 0) {
		log_error("Attempt to read an unopened device (%s).",
			  dev_name(where->dev));
		return 0;
	}

	/*
	 * Skip all writes in test mode.
	 */
	if (should_write && test_mode())
		return 1;

	if (where->size > SSIZE_MAX) {
		log_error("Read size too large: %" PRIu64, where->size);
		return 0;
	}

	if (lseek(fd, (off_t) where->start, SEEK_SET) < 0) {
		log_error("%s: lseek %" PRIu64 " failed: %s",
			  dev_name(where->dev), (uint64_t) where->start,
			  strerror(errno));
		return 0;
	}

	while (total < (size_t) where->size) {
		do
			n = should_write ?
			    write(fd, buffer, (size_t) where->size - total) :
			    read(fd, buffer, (size_t) where->size - total);
		while ((n < 0) && ((errno == EINTR) || (errno == EAGAIN)));

		if (n < 0)
			log_error_once("%s: %s failed after %" PRIu64 " of %" PRIu64
				       " at %" PRIu64 ": %s", dev_name(where->dev),
				       should_write ? "write" : "read",
				       (uint64_t) total,
				       (uint64_t) where->size,
				       (uint64_t) where->start, strerror(errno));

		if (n <= 0)
			break;

		total += n;
		buffer += n;
	}

	return (total == (size_t) where->size);
}

/*-----------------------------------------------------------------
 * LVM2 uses O_DIRECT when performing metadata io, which requires
 * block size aligned accesses.  If any io is not aligned we have
 * to perform the io via a bounce buffer, obviously this is quite
 * inefficient.
 *---------------------------------------------------------------*/

/*
 * Get the sector size from an _open_ device.
 */
static int _get_block_size(struct device *dev, unsigned int *size)
{
	const char *name = dev_name(dev);

	if (dev->block_size == -1) {
		if (ioctl(dev_fd(dev), BLKBSZGET, &dev->block_size) < 0) {
			log_sys_error("ioctl BLKBSZGET", name);
			return 0;
		}
		log_debug("%s: block size is %u bytes", name, dev->block_size);
	}

	*size = (unsigned int) dev->block_size;

	return 1;
}

/*
 * Widens a region to be an aligned region.
 */
static void _widen_region(unsigned int block_size, struct device_area *region,
			  struct device_area *result)
{
	uint64_t mask = block_size - 1, delta;
	memcpy(result, region, sizeof(*result));

	/* adjust the start */
	delta = result->start & mask;
	if (delta) {
		result->start -= delta;
		result->size += delta;
	}

	/* adjust the end */
	delta = (result->start + result->size) & mask;
	if (delta)
		result->size += block_size - delta;
}

static int _aligned_io(struct device_area *where, char *buffer,
		       int should_write)
{
	char *bounce, *bounce_buf;
	unsigned int block_size = 0;
	uintptr_t mask;
	struct device_area widened;
	int r = 0;

	if (!(where->dev->flags & DEV_REGULAR) &&
	    !_get_block_size(where->dev, &block_size))
		return_0;

	if (!block_size)
		block_size = lvm_getpagesize();

	_widen_region(block_size, where, &widened);

	/* Do we need to use a bounce buffer? */
	mask = block_size - 1;
	if (!memcmp(where, &widened, sizeof(widened)) &&
	    !((uintptr_t) buffer & mask))
		return _io(where, buffer, should_write);

	/* Allocate a bounce buffer with an extra block */
	if (!(bounce_buf = bounce = dm_malloc((size_t) widened.size + block_size))) {
		log_error("Bounce buffer malloc failed");
		return 0;
	}

	/*
	 * Realign start of bounce buffer (using the extra sector)
	 */
	if (((uintptr_t) bounce) & mask)
		bounce = (char *) ((((uintptr_t) bounce) + mask) & ~mask);

	/* channel the io through the bounce buffer */
	if (!_io(&widened, bounce, 0)) {
		if (!should_write)
			goto_out;
		/* FIXME pre-extend the file */
		memset(bounce, '\n', widened.size);
	}

	if (should_write) {
		memcpy(bounce + (where->start - widened.start), buffer,
		       (size_t) where->size);

		/* ... then we write */
		if (!(r = _io(&widened, bounce, 1)))
			stack;
			
		goto out;
	}

	memcpy(buffer, bounce + (where->start - widened.start),
	       (size_t) where->size);

	r = 1;

out:
	dm_free(bounce_buf);
	return r;
}

static int _dev_get_size_file(const struct device *dev, uint64_t *size)
{
	const char *name = dev_name(dev);
	struct stat info;

	if (stat(name, &info)) {
		log_sys_error("stat", name);
		return 0;
	}

	*size = info.st_size;
	*size >>= SECTOR_SHIFT;	/* Convert to sectors */

	log_very_verbose("%s: size is %" PRIu64 " sectors", name, *size);

	return 1;
}

static int _dev_get_size_dev(const struct device *dev, uint64_t *size)
{
	int fd;
	const char *name = dev_name(dev);

	if ((fd = open(name, O_RDONLY)) < 0) {
		log_sys_error("open", name);
		return 0;
	}

	if (ioctl(fd, BLKGETSIZE64, size) < 0) {
		log_sys_error("ioctl BLKGETSIZE64", name);
		if (close(fd))
			log_sys_error("close", name);
		return 0;
	}

	*size >>= BLKSIZE_SHIFT;	/* Convert to sectors */
	if (close(fd))
		log_sys_error("close", name);

	log_very_verbose("%s: size is %" PRIu64 " sectors", name, *size);

	return 1;
}

static int _dev_read_ahead_dev(struct device *dev, uint32_t *read_ahead)
{
	long read_ahead_long;

	if (dev->read_ahead != -1) {
		*read_ahead = (uint32_t) dev->read_ahead;
		return 1;
	}

	if (!dev_open(dev))
		return_0;

	if (ioctl(dev->fd, BLKRAGET, &read_ahead_long) < 0) {
		log_sys_error("ioctl BLKRAGET", dev_name(dev));
		if (!dev_close(dev))
			stack;
		return 0;
	}

	if (!dev_close(dev))
		stack;

	*read_ahead = (uint32_t) read_ahead_long;
	dev->read_ahead = read_ahead_long;

	log_very_verbose("%s: read_ahead is %u sectors",
			 dev_name(dev), *read_ahead);

	return 1;
}

static int _dev_discard_blocks(struct device *dev, uint64_t offset_bytes, uint64_t size_bytes)
{
	uint64_t discard_range[2];

	if (!dev_open(dev))
		return_0;

	discard_range[0] = offset_bytes;
	discard_range[1] = size_bytes;

	log_debug("Discarding %" PRIu64 " bytes offset %" PRIu64 " bytes on %s.",
		  size_bytes, offset_bytes, dev_name(dev));
	if (ioctl(dev->fd, BLKDISCARD, &discard_range) < 0) {
		log_error("%s: BLKDISCARD ioctl at offset %" PRIu64 " size %" PRIu64 " failed: %s.",
			  dev_name(dev), offset_bytes, size_bytes, strerror(errno));
		if (!dev_close(dev))
			stack;
		/* It doesn't matter if discard failed, so return success. */
		return 1;
	}

	if (!dev_close(dev))
		stack;

	return 1;
}

/*-----------------------------------------------------------------
 * Public functions
 *---------------------------------------------------------------*/

int dev_get_size(const struct device *dev, uint64_t *size)
{
	if (!dev)
		return 0;

	if ((dev->flags & DEV_REGULAR))
		return _dev_get_size_file(dev, size);
	else
		return _dev_get_size_dev(dev, size);
}

int dev_get_read_ahead(struct device *dev, uint32_t *read_ahead)
{
	if (!dev)
		return 0;

	if (dev->flags & DEV_REGULAR) {
		*read_ahead = 0;
		return 1;
	}

	return _dev_read_ahead_dev(dev, read_ahead);
}

int dev_discard_blocks(struct device *dev, uint64_t offset_bytes, uint64_t size_bytes)
{
	if (!dev)
		return 0;

	if (dev->flags & DEV_REGULAR)
		return 1;

	return _dev_discard_blocks(dev, offset_bytes, size_bytes);
}

/* FIXME Unused
int dev_get_sectsize(struct device *dev, uint32_t *size)
{
	int fd;
	int s;
	const char *name = dev_name(dev);

	if ((fd = open(name, O_RDONLY)) < 0) {
		log_sys_error("open", name);
		return 0;
	}

	if (ioctl(fd, BLKSSZGET, &s) < 0) {
		log_sys_error("ioctl BLKSSZGET", name);
		if (close(fd))
			log_sys_error("close", name);
		return 0;
	}

	if (close(fd))
		log_sys_error("close", name);

	*size = (uint32_t) s;

	log_very_verbose("%s: sector size is %" PRIu32 " bytes", name, *size);

	return 1;
}
*/

void dev_flush(struct device *dev)
{
	if (!(dev->flags & DEV_REGULAR) && ioctl(dev->fd, BLKFLSBUF, 0) >= 0)
		return;

	if (fsync(dev->fd) >= 0)
		return;

	sync();
}

int dev_open_flags(struct device *dev, int flags, int direct, int quiet)
{
	struct stat buf;
	const char *name;
	int need_excl = 0, need_rw = 0;

	if ((flags & O_ACCMODE) == O_RDWR)
		need_rw = 1;

	if ((flags & O_EXCL))
		need_excl = 1;

	if (dev->fd >= 0) {
		if (((dev->flags & DEV_OPENED_RW) || !need_rw) &&
		    ((dev->flags & DEV_OPENED_EXCL) || !need_excl)) {
			dev->open_count++;
			return 1;
		}

		if (dev->open_count && !need_excl) {
			log_debug("%s already opened read-only. Upgrading "
				  "to read-write.", dev_name(dev));
			dev->open_count++;
		}

		dev_close_immediate(dev);
	}

	if (critical_section())
		/* FIXME Make this log_error */
		log_verbose("dev_open(%s) called while suspended",
			 dev_name(dev));

	if (dev->flags & DEV_REGULAR)
		name = dev_name(dev);
	else if (!(name = dev_name_confirmed(dev, quiet)))
		return_0;

	if (!(dev->flags & DEV_REGULAR)) {
		if (stat(name, &buf) < 0) {
			log_sys_error("%s: stat failed", name);
			return 0;
		}
		if (buf.st_rdev != dev->dev) {
			log_error("%s: device changed", name);
			return 0;
		}
	}

#ifdef O_DIRECT_SUPPORT
	if (direct) {
		if (!(dev->flags & DEV_O_DIRECT_TESTED))
			dev->flags |= DEV_O_DIRECT;

		if ((dev->flags & DEV_O_DIRECT))
			flags |= O_DIRECT;
	}
#endif

#ifdef O_NOATIME
	/* Don't update atime on device inodes */
	if (!(dev->flags & DEV_REGULAR))
		flags |= O_NOATIME;
#endif

	if ((dev->fd = open(name, flags, 0777)) < 0) {
#ifdef O_DIRECT_SUPPORT
		if (direct && !(dev->flags & DEV_O_DIRECT_TESTED)) {
			flags &= ~O_DIRECT;
			if ((dev->fd = open(name, flags, 0777)) >= 0) {
				dev->flags &= ~DEV_O_DIRECT;
				log_debug("%s: Not using O_DIRECT", name);
				goto opened;
			}
		}
#endif
		if (quiet)
			log_sys_debug("open", name);
		else
			log_sys_error("open", name);
		return 0;
	}

#ifdef O_DIRECT_SUPPORT
      opened:
	if (direct)
		dev->flags |= DEV_O_DIRECT_TESTED;
#endif
	dev->open_count++;
	dev->flags &= ~DEV_ACCESSED_W;

	if (need_rw)
		dev->flags |= DEV_OPENED_RW;
	else
		dev->flags &= ~DEV_OPENED_RW;

	if (need_excl)
		dev->flags |= DEV_OPENED_EXCL;
	else
		dev->flags &= ~DEV_OPENED_EXCL;

	if (!(dev->flags & DEV_REGULAR) &&
	    ((fstat(dev->fd, &buf) < 0) || (buf.st_rdev != dev->dev))) {
		log_error("%s: fstat failed: Has device name changed?", name);
		dev_close_immediate(dev);
		return 0;
	}

#ifndef O_DIRECT_SUPPORT
	if (!(dev->flags & DEV_REGULAR))
		dev_flush(dev);
#endif

	if ((flags & O_CREAT) && !(flags & O_TRUNC))
		dev->end = lseek(dev->fd, (off_t) 0, SEEK_END);

	dm_list_add(&_open_devices, &dev->open_list);

	log_debug("Opened %s %s%s%s", dev_name(dev),
		  dev->flags & DEV_OPENED_RW ? "RW" : "RO",
		  dev->flags & DEV_OPENED_EXCL ? " O_EXCL" : "",
		  dev->flags & DEV_O_DIRECT ? " O_DIRECT" : "");

	return 1;
}

int dev_open_quiet(struct device *dev)
{
	return dev_open_flags(dev, O_RDWR, 1, 1);
}

int dev_open(struct device *dev)
{
	return dev_open_flags(dev, O_RDWR, 1, 0);
}

int dev_open_readonly(struct device *dev)
{
	return dev_open_flags(dev, O_RDONLY, 1, 0);
}

int dev_open_readonly_buffered(struct device *dev)
{
	return dev_open_flags(dev, O_RDONLY, 0, 0);
}

int dev_open_readonly_quiet(struct device *dev)
{
	return dev_open_flags(dev, O_RDONLY, 1, 1);
}

int dev_test_excl(struct device *dev)
{
	int flags;
	int r;

	flags = vg_write_lock_held() ? O_RDWR : O_RDONLY;
	flags |= O_EXCL;

	r = dev_open_flags(dev, flags, 1, 1);
	if (r)
		dev_close_immediate(dev);

	return r;
}

static void _close(struct device *dev)
{
	if (close(dev->fd))
		log_sys_error("close", dev_name(dev));
	dev->fd = -1;
	dev->block_size = -1;
	dm_list_del(&dev->open_list);

	log_debug("Closed %s", dev_name(dev));

	if (dev->flags & DEV_ALLOCED) {
		dm_free((void *) dm_list_item(dev->aliases.n, struct str_list)->
			 str);
		dm_free(dev->aliases.n);
		dm_free(dev);
	}
}

static int _dev_close(struct device *dev, int immediate)
{
	struct lvmcache_info *info;

	if (dev->fd < 0) {
		log_error("Attempt to close device '%s' "
			  "which is not open.", dev_name(dev));
		return 0;
	}

#ifndef O_DIRECT_SUPPORT
	if (dev->flags & DEV_ACCESSED_W)
		dev_flush(dev);
#endif

	if (dev->open_count > 0)
		dev->open_count--;

	if (immediate && dev->open_count)
		log_debug("%s: Immediate close attempt while still referenced",
			  dev_name(dev));

	/* Close unless device is known to belong to a locked VG */
	if (immediate ||
	    (dev->open_count < 1 &&
	     (!(info = info_from_pvid(dev->pvid, 0)) ||
	      !info->vginfo ||
	      !vgname_is_locked(info->vginfo->vgname))))
		_close(dev);

	return 1;
}

int dev_close(struct device *dev)
{
	return _dev_close(dev, 0);
}

int dev_close_immediate(struct device *dev)
{
	return _dev_close(dev, 1);
}

void dev_close_all(void)
{
	struct dm_list *doh, *doht;
	struct device *dev;

	dm_list_iterate_safe(doh, doht, &_open_devices) {
		dev = dm_list_struct_base(doh, struct device, open_list);
		if (dev->open_count < 1)
			_close(dev);
	}
}

static inline int _dev_is_valid(struct device *dev)
{
	return (dev->max_error_count == NO_DEV_ERROR_COUNT_LIMIT ||
		dev->error_count < dev->max_error_count);
}

static void _dev_inc_error_count(struct device *dev)
{
	if (++dev->error_count == dev->max_error_count)
		log_warn("WARNING: Error counts reached a limit of %d. "
			 "Device %s was disabled",
			 dev->max_error_count, dev_name(dev));
}

int dev_read(struct device *dev, uint64_t offset, size_t len, void *buffer)
{
	struct device_area where;
	int ret;

	if (!dev->open_count)
		return_0;

	if (!_dev_is_valid(dev))
		return 0;

	where.dev = dev;
	where.start = offset;
	where.size = len;

	ret = _aligned_io(&where, buffer, 0);
	if (!ret)
		_dev_inc_error_count(dev);

	return ret;
}

/*
 * Read from 'dev' into 'buf', possibly in 2 distinct regions, denoted
 * by (offset,len) and (offset2,len2).  Thus, the total size of
 * 'buf' should be len+len2.
 */
int dev_read_circular(struct device *dev, uint64_t offset, size_t len,
		      uint64_t offset2, size_t len2, char *buf)
{
	if (!dev_read(dev, offset, len, buf)) {
		log_error("Read from %s failed", dev_name(dev));
		return 0;
	}

	/*
	 * The second region is optional, and allows for
	 * a circular buffer on the device.
	 */
	if (!len2)
		return 1;

	if (!dev_read(dev, offset2, len2, buf + len)) {
		log_error("Circular read from %s failed",
			  dev_name(dev));
		return 0;
	}

	return 1;
}

/* FIXME If O_DIRECT can't extend file, dev_extend first; dev_truncate after.
 *       But fails if concurrent processes writing
 */

/* FIXME pre-extend the file */
int dev_append(struct device *dev, size_t len, char *buffer)
{
	int r;

	if (!dev->open_count)
		return_0;

	r = dev_write(dev, dev->end, len, buffer);
	dev->end += (uint64_t) len;

#ifndef O_DIRECT_SUPPORT
	dev_flush(dev);
#endif
	return r;
}

int dev_write(struct device *dev, uint64_t offset, size_t len, void *buffer)
{
	struct device_area where;
	int ret;

	if (!dev->open_count)
		return_0;

	if (!_dev_is_valid(dev))
		return 0;

	where.dev = dev;
	where.start = offset;
	where.size = len;

	dev->flags |= DEV_ACCESSED_W;

	ret = _aligned_io(&where, buffer, 1);
	if (!ret)
		_dev_inc_error_count(dev);

	return ret;
}

int dev_set(struct device *dev, uint64_t offset, size_t len, int value)
{
	size_t s;
	char buffer[4096] __attribute__((aligned(8)));

	if (!dev_open(dev))
		return_0;

	if ((offset % SECTOR_SIZE) || (len % SECTOR_SIZE))
		log_debug("Wiping %s at %" PRIu64 " length %" PRIsize_t,
			  dev_name(dev), offset, len);
	else
		log_debug("Wiping %s at sector %" PRIu64 " length %" PRIsize_t
			  " sectors", dev_name(dev), offset >> SECTOR_SHIFT,
			  len >> SECTOR_SHIFT);

	memset(buffer, value, sizeof(buffer));
	while (1) {
		s = len > sizeof(buffer) ? sizeof(buffer) : len;
		if (!dev_write(dev, offset, s, buffer))
			break;

		len -= s;
		if (!len)
			break;

		offset += s;
	}

	dev->flags |= DEV_ACCESSED_W;

	if (!dev_close(dev))
		stack;

	return (len == 0);
}
