/*
   +----------------------------------------------------------------------+
   | PHP Version 7                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2015 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Wez Furlong <wez@thebrainroom.com>                          |
   +----------------------------------------------------------------------+
 */

/* $Id$ */

#include "php.h"
#include "php_globals.h"
#include "php_network.h"
#include "php_open_temporary_file.h"
#include "ext/standard/file.h"
#include "ext/standard/flock_compat.h"
#include "ext/standard/php_filestat.h"
#include <stddef.h>
#include <fcntl.h>
#if HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#if HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#include "SAPI.h"

#include "php_streams_int.h"
#ifdef PHP_WIN32
# include "win32/winutil.h"
# include "win32/time.h"
#endif

#define php_stream_fopen_from_fd_int(fd, mode, persistent_id)	_php_stream_fopen_from_fd_int((fd), (mode), (persistent_id) STREAMS_CC)
#define php_stream_fopen_from_fd_int_rel(fd, mode, persistent_id)	 _php_stream_fopen_from_fd_int((fd), (mode), (persistent_id) STREAMS_REL_CC)
#define php_stream_fopen_from_file_int(file, mode)	_php_stream_fopen_from_file_int((file), (mode) STREAMS_CC)
#define php_stream_fopen_from_file_int_rel(file, mode)	 _php_stream_fopen_from_file_int((file), (mode) STREAMS_REL_CC)

#if !defined(WINDOWS) && !defined(NETWARE)
extern int php_get_uid_by_name(const char *name, uid_t *uid);
extern int php_get_gid_by_name(const char *name, gid_t *gid);
#endif

#if defined(PHP_WIN32)
# define PLAIN_WRAP_BUF_SIZE(st) (((st) > UINT_MAX) ? UINT_MAX : (unsigned int)(st))
#else
# define PLAIN_WRAP_BUF_SIZE(st) (st)
#endif

/* parse standard "fopen" modes into open() flags */
PHPAPI int php_stream_parse_fopen_modes(const char *mode, int *open_flags)
{
	int flags;

	switch (mode[0]) {
		case 'r':
			flags = 0;
			break;
		case 'w':
			flags = O_TRUNC|O_CREAT;
			break;
		case 'a':
			flags = O_CREAT|O_APPEND;
			break;
		case 'x':
			flags = O_CREAT|O_EXCL;
			break;
		case 'c':
			flags = O_CREAT;
			break;
		default:
			/* unknown mode */
			return FAILURE;
	}

	if (strchr(mode, '+')) {
		flags |= O_RDWR;
	} else if (flags) {
		flags |= O_WRONLY;
	} else {
		flags |= O_RDONLY;
	}

#if defined(O_NONBLOCK)
	if (strchr(mode, 'n')) {
		flags |= O_NONBLOCK;
	}
#endif

#if defined(_O_TEXT) && defined(O_BINARY)
	if (strchr(mode, 't')) {
		flags |= _O_TEXT;
	} else {
		flags |= O_BINARY;
	}
#endif

	*open_flags = flags;
	return SUCCESS;
}


/* {{{ ------- STDIO stream implementation -------*/

typedef struct {
	FILE *file;
	int fd;					/* underlying file descriptor */
	unsigned is_process_pipe:1;	/* use pclose instead of fclose */
	unsigned is_pipe:1;			/* don't try and seek */
	unsigned cached_fstat:1;	/* sb is valid */
	unsigned _reserved:29;

	int lock_flag;			/* stores the lock state */
	zend_string *temp_name;	/* if non-null, this is the path to a temporary file that
							 * is to be deleted when the stream is closed */
#if HAVE_FLUSHIO
	char last_op;
#endif

#if HAVE_MMAP
	char *last_mapped_addr;
	size_t last_mapped_len;
#endif
#ifdef PHP_WIN32
	char *last_mapped_addr;
	HANDLE file_mapping;
#endif

	zend_stat_t sb;
} php_stdio_stream_data;
#define PHP_STDIOP_GET_FD(anfd, data)	anfd = (data)->file ? fileno((data)->file) : (data)->fd

static int do_fstat(php_stdio_stream_data *d, int force)
{
	if (!d->cached_fstat || force) {
		int fd;
		int r;

		PHP_STDIOP_GET_FD(fd, d);
		r = zend_fstat(fd, &d->sb);
		d->cached_fstat = r == 0;

		return r;
	}
	return 0;
}

static php_stream *_php_stream_fopen_from_fd_int(int fd, const char *mode, const char *persistent_id STREAMS_DC)
{
	php_stdio_stream_data *self;

	self = pemalloc_rel_orig(sizeof(*self), persistent_id);
	memset(self, 0, sizeof(*self));
	self->file = NULL;
	self->is_pipe = 0;
	self->lock_flag = LOCK_UN;
	self->is_process_pipe = 0;
	self->temp_name = NULL;
	self->fd = fd;

	return php_stream_alloc_rel(&php_stream_stdio_ops, self, persistent_id, mode);
}

static php_stream *_php_stream_fopen_from_file_int(FILE *file, const char *mode STREAMS_DC)
{
	php_stdio_stream_data *self;

	self = emalloc_rel_orig(sizeof(*self));
	memset(self, 0, sizeof(*self));
	self->file = file;
	self->is_pipe = 0;
	self->lock_flag = LOCK_UN;
	self->is_process_pipe = 0;
	self->temp_name = NULL;
	self->fd = fileno(file);

	return php_stream_alloc_rel(&php_stream_stdio_ops, self, 0, mode);
}

PHPAPI php_stream *_php_stream_fopen_temporary_file(const char *dir, const char *pfx, zend_string **opened_path_ptr STREAMS_DC)
{
	zend_string *opened_path = NULL;
	int fd;

	fd = php_open_temporary_fd(dir, pfx, &opened_path);
	if (fd != -1)	{
		php_stream *stream;

		if (opened_path_ptr) {
			*opened_path_ptr = opened_path;
		}

		stream = php_stream_fopen_from_fd_int_rel(fd, "r+b", NULL);
		if (stream) {
			php_stdio_stream_data *self = (php_stdio_stream_data*)stream->abstract;
			stream->wrapper = &php_plain_files_wrapper;
			stream->orig_path = estrndup(opened_path->val, opened_path->len);

			self->temp_name = opened_path;
			self->lock_flag = LOCK_UN;

			return stream;
		}
		close(fd);

		php_error_docref(NULL, E_WARNING, "unable to allocate stream");

		return NULL;
	}
	return NULL;
}

PHPAPI php_stream *_php_stream_fopen_tmpfile(int dummy STREAMS_DC)
{
	return php_stream_fopen_temporary_file(NULL, "php", NULL);
}

PHPAPI php_stream *_php_stream_fopen_from_fd(int fd, const char *mode, const char *persistent_id STREAMS_DC)
{
	php_stream *stream = php_stream_fopen_from_fd_int_rel(fd, mode, persistent_id);

	if (stream) {
		php_stdio_stream_data *self = (php_stdio_stream_data*)stream->abstract;

#ifdef S_ISFIFO
		/* detect if this is a pipe */
		if (self->fd >= 0) {
			self->is_pipe = (do_fstat(self, 0) == 0 && S_ISFIFO(self->sb.st_mode)) ? 1 : 0;
		}
#elif defined(PHP_WIN32)
		{
			zend_uintptr_t handle = _get_osfhandle(self->fd);

			if (handle != (zend_uintptr_t)INVALID_HANDLE_VALUE) {
				self->is_pipe = GetFileType((HANDLE)handle) == FILE_TYPE_PIPE;
			}
		}
#endif

		if (self->is_pipe) {
			stream->flags |= PHP_STREAM_FLAG_NO_SEEK;
		} else {
			stream->position = zend_lseek(self->fd, 0, SEEK_CUR);
#ifdef ESPIPE
			if (stream->position == (zend_off_t)-1 && errno == ESPIPE) {
				stream->position = 0;
				stream->flags |= PHP_STREAM_FLAG_NO_SEEK;
				self->is_pipe = 1;
			}
#endif
		}
	}

	return stream;
}

PHPAPI php_stream *_php_stream_fopen_from_file(FILE *file, const char *mode STREAMS_DC)
{
	php_stream *stream = php_stream_fopen_from_file_int_rel(file, mode);

	if (stream) {
		php_stdio_stream_data *self = (php_stdio_stream_data*)stream->abstract;

#ifdef S_ISFIFO
		/* detect if this is a pipe */
		if (self->fd >= 0) {
			self->is_pipe = (do_fstat(self, 0) == 0 && S_ISFIFO(self->sb.st_mode)) ? 1 : 0;
		}
#elif defined(PHP_WIN32)
		{
			zend_uintptr_t handle = _get_osfhandle(self->fd);

			if (handle != (zend_uintptr_t)INVALID_HANDLE_VALUE) {
				self->is_pipe = GetFileType((HANDLE)handle) == FILE_TYPE_PIPE;
			}
		}
#endif

		if (self->is_pipe) {
			stream->flags |= PHP_STREAM_FLAG_NO_SEEK;
		} else {
			stream->position = zend_ftell(file);
		}
	}

	return stream;
}

PHPAPI php_stream *_php_stream_fopen_from_pipe(FILE *file, const char *mode STREAMS_DC)
{
	php_stdio_stream_data *self;
	php_stream *stream;

	self = emalloc_rel_orig(sizeof(*self));
	memset(self, 0, sizeof(*self));
	self->file = file;
	self->is_pipe = 1;
	self->lock_flag = LOCK_UN;
	self->is_process_pipe = 1;
	self->fd = fileno(file);
	self->temp_name = NULL;

	stream = php_stream_alloc_rel(&php_stream_stdio_ops, self, 0, mode);
	stream->flags |= PHP_STREAM_FLAG_NO_SEEK;
	return stream;
}

static size_t php_stdiop_write(php_stream *stream, const char *buf, size_t count)
{
	php_stdio_stream_data *data = (php_stdio_stream_data*)stream->abstract;

	assert(data != NULL);

	if (data->fd >= 0) {
#ifdef PHP_WIN32
		int bytes_written = write(data->fd, buf, (unsigned int)count);
#else
		int bytes_written = write(data->fd, buf, count);
#endif
		if (bytes_written < 0) return 0;
		return (size_t) bytes_written;
	} else {

#if HAVE_FLUSHIO
		if (!data->is_pipe && data->last_op == 'r') {
			zend_fseek(data->file, 0, SEEK_CUR);
		}
		data->last_op = 'w';
#endif

		return fwrite(buf, 1, count, data->file);
	}
}

static size_t php_stdiop_read(php_stream *stream, char *buf, size_t count)
{
	php_stdio_stream_data *data = (php_stdio_stream_data*)stream->abstract;
	size_t ret;

	assert(data != NULL);

	if (data->fd >= 0) {
#ifdef PHP_WIN32
		php_stdio_stream_data *self = (php_stdio_stream_data*)stream->abstract;

		if (self->is_pipe || self->is_process_pipe) {
			HANDLE ph = (HANDLE)_get_osfhandle(data->fd);
			int retry = 0;
			DWORD avail_read = 0;

			do {
				/* Look ahead to get the available data amount to read. Do the same
					as read() does, however not blocking forever. In case it failed,
					no data will be read (better than block). */
				if (!PeekNamedPipe(ph, NULL, 0, NULL, &avail_read, NULL)) {
					break;
				}
				/* If there's nothing to read, wait in 100ms periods. */
				if (0 == avail_read) {
					usleep(100000);
				}
			} while (0 == avail_read && retry++ < 320);

			/* Reduce the required data amount to what is available, otherwise read()
				will block.*/
			if (avail_read < count) {
				count = avail_read;
			}
		}
#endif
		ret = read(data->fd, buf,  PLAIN_WRAP_BUF_SIZE(count));

		if (ret == (size_t)-1 && errno == EINTR) {
			/* Read was interrupted, retry once,
			   If read still fails, giveup with feof==0
			   so script can retry if desired */
			ret = read(data->fd, buf,  PLAIN_WRAP_BUF_SIZE(count));
		}

		stream->eof = (ret == 0 || (ret == (size_t)-1 && errno != EWOULDBLOCK && errno != EINTR && errno != EBADF));

	} else {
#if HAVE_FLUSHIO
		if (!data->is_pipe && data->last_op == 'w')
			zend_fseek(data->file, 0, SEEK_CUR);
		data->last_op = 'r';
#endif

		ret = fread(buf, 1, count, data->file);

		stream->eof = feof(data->file);
	}
	return ret;
}

static int php_stdiop_close(php_stream *stream, int close_handle)
{
	int ret;
	php_stdio_stream_data *data = (php_stdio_stream_data*)stream->abstract;

	assert(data != NULL);

#if HAVE_MMAP
	if (data->last_mapped_addr) {
		munmap(data->last_mapped_addr, data->last_mapped_len);
		data->last_mapped_addr = NULL;
	}
#elif defined(PHP_WIN32)
	if (data->last_mapped_addr) {
		UnmapViewOfFile(data->last_mapped_addr);
		data->last_mapped_addr = NULL;
	}
	if (data->file_mapping) {
		CloseHandle(data->file_mapping);
		data->file_mapping = NULL;
	}
#endif

	if (close_handle) {
		if (data->file) {
			if (data->is_process_pipe) {
				errno = 0;
				ret = pclose(data->file);

#if HAVE_SYS_WAIT_H
				if (WIFEXITED(ret)) {
					ret = WEXITSTATUS(ret);
				}
#endif
			} else {
				ret = fclose(data->file);
				data->file = NULL;
			}
		} else if (data->fd != -1) {
			ret = close(data->fd);
			data->fd = -1;
		} else {
			return 0; /* everything should be closed already -> success */
		}
		if (data->temp_name) {
			unlink(data->temp_name->val);
			/* temporary streams are never persistent */
			zend_string_release(data->temp_name);
			data->temp_name = NULL;
		}
	} else {
		ret = 0;
		data->file = NULL;
		data->fd = -1;
	}

	pefree(data, stream->is_persistent);

	return ret;
}

static int php_stdiop_flush(php_stream *stream)
{
	php_stdio_stream_data *data = (php_stdio_stream_data*)stream->abstract;

	assert(data != NULL);

	/*
	 * stdio buffers data in user land. By calling fflush(3), this
	 * data is send to the kernel using write(2). fsync'ing is
	 * something completely different.
	 */
	if (data->file) {
		return fflush(data->file);
	}
	return 0;
}

static int php_stdiop_seek(php_stream *stream, zend_off_t offset, int whence, zend_off_t *newoffset)
{
	php_stdio_stream_data *data = (php_stdio_stream_data*)stream->abstract;
	int ret;

	assert(data != NULL);

	if (data->is_pipe) {
		php_error_docref(NULL, E_WARNING, "cannot seek on a pipe");
		return -1;
	}

	if (data->fd >= 0) {
		zend_off_t result;

		result = zend_lseek(data->fd, offset, whence);
		if (result == (zend_off_t)-1)
			return -1;

		*newoffset = result;
		return 0;

	} else {
		ret = zend_fseek(data->file, offset, whence);
		*newoffset = zend_ftell(data->file);
		return ret;
	}
}

static int php_stdiop_cast(php_stream *stream, int castas, void **ret)
{
	php_socket_t fd;
	php_stdio_stream_data *data = (php_stdio_stream_data*) stream->abstract;

	assert(data != NULL);

	/* as soon as someone touches the stdio layer, buffering may ensue,
	 * so we need to stop using the fd directly in that case */

	switch (castas)	{
		case PHP_STREAM_AS_STDIO:
			if (ret) {

				if (data->file == NULL) {
					/* we were opened as a plain file descriptor, so we
					 * need fdopen now */
					char fixed_mode[5];
					php_stream_mode_sanitize_fdopen_fopencookie(stream, fixed_mode);
					data->file = fdopen(data->fd, fixed_mode);
					if (data->file == NULL) {
						return FAILURE;
					}
				}

				*(FILE**)ret = data->file;
				data->fd = SOCK_ERR;
			}
			return SUCCESS;

		case PHP_STREAM_AS_FD_FOR_SELECT:
			PHP_STDIOP_GET_FD(fd, data);
			if (SOCK_ERR == fd) {
				return FAILURE;
			}
			if (ret) {
				*(php_socket_t *)ret = fd;
			}
			return SUCCESS;

		case PHP_STREAM_AS_FD:
			PHP_STDIOP_GET_FD(fd, data);

			if (SOCK_ERR == fd) {
				return FAILURE;
			}
			if (data->file) {
				fflush(data->file);
			}
			if (ret) {
				*(php_socket_t *)ret = fd;
			}
			return SUCCESS;
		default:
			return FAILURE;
	}
}

static int php_stdiop_stat(php_stream *stream, php_stream_statbuf *ssb)
{
	int ret;
	php_stdio_stream_data *data = (php_stdio_stream_data*) stream->abstract;

	assert(data != NULL);
	if((ret = do_fstat(data, 1)) == 0) {
		memcpy(&ssb->sb, &data->sb, sizeof(ssb->sb));
	}

	return ret;
}

static int php_stdiop_set_option(php_stream *stream, int option, int value, void *ptrparam)
{
	php_stdio_stream_data *data = (php_stdio_stream_data*) stream->abstract;
	size_t size;
	int fd;
#ifdef O_NONBLOCK
	/* FIXME: make this work for win32 */
	int flags;
	int oldval;
#endif

	PHP_STDIOP_GET_FD(fd, data);

	switch(option) {
		case PHP_STREAM_OPTION_BLOCKING:
			if (fd == -1)
				return -1;
#ifdef O_NONBLOCK
			flags = fcntl(fd, F_GETFL, 0);
			oldval = (flags & O_NONBLOCK) ? 0 : 1;
			if (value)
				flags &= ~O_NONBLOCK;
			else
				flags |= O_NONBLOCK;

			if (-1 == fcntl(fd, F_SETFL, flags))
				return -1;
			return oldval;
#else
			return -1; /* not yet implemented */
#endif

		case PHP_STREAM_OPTION_WRITE_BUFFER:

			if (data->file == NULL) {
				return -1;
			}

			if (ptrparam)
				size = *(size_t *)ptrparam;
			else
				size = BUFSIZ;

			switch(value) {
				case PHP_STREAM_BUFFER_NONE:
					return setvbuf(data->file, NULL, _IONBF, 0);

				case PHP_STREAM_BUFFER_LINE:
					return setvbuf(data->file, NULL, _IOLBF, size);

				case PHP_STREAM_BUFFER_FULL:
					return setvbuf(data->file, NULL, _IOFBF, size);

				default:
					return -1;
			}
			break;

		case PHP_STREAM_OPTION_LOCKING:
			if (fd == -1) {
				return -1;
			}

			if ((zend_uintptr_t) ptrparam == PHP_STREAM_LOCK_SUPPORTED) {
				return 0;
			}

			if (!flock(fd, value)) {
				data->lock_flag = value;
				return 0;
			} else {
				return -1;
			}
			break;

		case PHP_STREAM_OPTION_MMAP_API:
#if HAVE_MMAP
			{
				php_stream_mmap_range *range = (php_stream_mmap_range*)ptrparam;
				int prot, flags;

				switch (value) {
					case PHP_STREAM_MMAP_SUPPORTED:
						return fd == -1 ? PHP_STREAM_OPTION_RETURN_ERR : PHP_STREAM_OPTION_RETURN_OK;

					case PHP_STREAM_MMAP_MAP_RANGE:
						if(do_fstat(data, 1) != 0) {
							return PHP_STREAM_OPTION_RETURN_ERR;
						}
						if (range->length == 0 && range->offset > 0 && range->offset < data->sb.st_size) {
							range->length = data->sb.st_size - range->offset;
						}
						if (range->length == 0 || range->length > data->sb.st_size) {
							range->length = data->sb.st_size;
						}
						if (range->offset >= data->sb.st_size) {
							range->offset = data->sb.st_size;
							range->length = 0;
						}
						switch (range->mode) {
							case PHP_STREAM_MAP_MODE_READONLY:
								prot = PROT_READ;
								flags = MAP_PRIVATE;
								break;
							case PHP_STREAM_MAP_MODE_READWRITE:
								prot = PROT_READ | PROT_WRITE;
								flags = MAP_PRIVATE;
								break;
							case PHP_STREAM_MAP_MODE_SHARED_READONLY:
								prot = PROT_READ;
								flags = MAP_SHARED;
								break;
							case PHP_STREAM_MAP_MODE_SHARED_READWRITE:
								prot = PROT_READ | PROT_WRITE;
								flags = MAP_SHARED;
								break;
							default:
								return PHP_STREAM_OPTION_RETURN_ERR;
						}
						range->mapped = (char*)mmap(NULL, range->length, prot, flags, fd, range->offset);
						if (range->mapped == (char*)MAP_FAILED) {
							range->mapped = NULL;
							return PHP_STREAM_OPTION_RETURN_ERR;
						}
						/* remember the mapping */
						data->last_mapped_addr = range->mapped;
						data->last_mapped_len = range->length;
						return PHP_STREAM_OPTION_RETURN_OK;

					case PHP_STREAM_MMAP_UNMAP:
						if (data->last_mapped_addr) {
							munmap(data->last_mapped_addr, data->last_mapped_len);
							data->last_mapped_addr = NULL;

							return PHP_STREAM_OPTION_RETURN_OK;
						}
						return PHP_STREAM_OPTION_RETURN_ERR;
				}
			}
#elif defined(PHP_WIN32)
			{
				php_stream_mmap_range *range = (php_stream_mmap_range*)ptrparam;
				HANDLE hfile = (HANDLE)_get_osfhandle(fd);
				DWORD prot, acc, loffs = 0, delta = 0;

				switch (value) {
					case PHP_STREAM_MMAP_SUPPORTED:
						return hfile == INVALID_HANDLE_VALUE ? PHP_STREAM_OPTION_RETURN_ERR : PHP_STREAM_OPTION_RETURN_OK;

					case PHP_STREAM_MMAP_MAP_RANGE:
						switch (range->mode) {
							case PHP_STREAM_MAP_MODE_READONLY:
								prot = PAGE_READONLY;
								acc = FILE_MAP_READ;
								break;
							case PHP_STREAM_MAP_MODE_READWRITE:
								prot = PAGE_READWRITE;
								acc = FILE_MAP_READ | FILE_MAP_WRITE;
								break;
							case PHP_STREAM_MAP_MODE_SHARED_READONLY:
								prot = PAGE_READONLY;
								acc = FILE_MAP_READ;
								/* TODO: we should assign a name for the mapping */
								break;
							case PHP_STREAM_MAP_MODE_SHARED_READWRITE:
								prot = PAGE_READWRITE;
								acc = FILE_MAP_READ | FILE_MAP_WRITE;
								/* TODO: we should assign a name for the mapping */
								break;
							default:
								return PHP_STREAM_OPTION_RETURN_ERR;
						}

						/* create a mapping capable of viewing the whole file (this costs no real resources) */
						data->file_mapping = CreateFileMapping(hfile, NULL, prot, 0, 0, NULL);

						if (data->file_mapping == NULL) {
							return PHP_STREAM_OPTION_RETURN_ERR;
						}

						size = GetFileSize(hfile, NULL);
						if (range->length == 0 && range->offset > 0 && range->offset < size) {
							range->length = size - range->offset;
						}
						if (range->length == 0 || range->length > size) {
							range->length = size;
						}
						if (range->offset >= size) {
							range->offset = size;
							range->length = 0;
						}

						/* figure out how big a chunk to map to be able to view the part that we need */
						if (range->offset != 0) {
							SYSTEM_INFO info;
							DWORD gran;

							GetSystemInfo(&info);
							gran = info.dwAllocationGranularity;
							loffs = ((DWORD)range->offset / gran) * gran;
							delta = (DWORD)range->offset - loffs;
						}

						data->last_mapped_addr = MapViewOfFile(data->file_mapping, acc, 0, loffs, range->length + delta);

						if (data->last_mapped_addr) {
							/* give them back the address of the start offset they requested */
							range->mapped = data->last_mapped_addr + delta;
							return PHP_STREAM_OPTION_RETURN_OK;
						}

						CloseHandle(data->file_mapping);
						data->file_mapping = NULL;

						return PHP_STREAM_OPTION_RETURN_ERR;

					case PHP_STREAM_MMAP_UNMAP:
						if (data->last_mapped_addr) {
							UnmapViewOfFile(data->last_mapped_addr);
							data->last_mapped_addr = NULL;
							CloseHandle(data->file_mapping);
							data->file_mapping = NULL;
							return PHP_STREAM_OPTION_RETURN_OK;
						}
						return PHP_STREAM_OPTION_RETURN_ERR;

					default:
						return PHP_STREAM_OPTION_RETURN_ERR;
				}
			}

#endif
			return PHP_STREAM_OPTION_RETURN_NOTIMPL;

		case PHP_STREAM_OPTION_TRUNCATE_API:
			switch (value) {
				case PHP_STREAM_TRUNCATE_SUPPORTED:
					return fd == -1 ? PHP_STREAM_OPTION_RETURN_ERR : PHP_STREAM_OPTION_RETURN_OK;

				case PHP_STREAM_TRUNCATE_SET_SIZE: {
					ptrdiff_t new_size = *(ptrdiff_t*)ptrparam;
					if (new_size < 0) {
						return PHP_STREAM_OPTION_RETURN_ERR;
					}
					return ftruncate(fd, new_size) == 0 ? PHP_STREAM_OPTION_RETURN_OK : PHP_STREAM_OPTION_RETURN_ERR;
				}
			}

		default:
			return PHP_STREAM_OPTION_RETURN_NOTIMPL;
	}
}

PHPAPI php_stream_ops	php_stream_stdio_ops = {
	php_stdiop_write, php_stdiop_read,
	php_stdiop_close, php_stdiop_flush,
	"STDIO",
	php_stdiop_seek,
	php_stdiop_cast,
	php_stdiop_stat,
	php_stdiop_set_option
};
/* }}} */

/* {{{ plain files opendir/readdir implementation */
static size_t php_plain_files_dirstream_read(php_stream *stream, char *buf, size_t count)
{
	DIR *dir = (DIR*)stream->abstract;
	/* avoid libc5 readdir problems */
	char entry[sizeof(struct dirent)+MAXPATHLEN];
	struct dirent *result = (struct dirent *)&entry;
	php_stream_dirent *ent = (php_stream_dirent*)buf;

	/* avoid problems if someone mis-uses the stream */
	if (count != sizeof(php_stream_dirent))
		return 0;

	if (php_readdir_r(dir, (struct dirent *)entry, &result) == 0 && result) {
		PHP_STRLCPY(ent->d_name, result->d_name, sizeof(ent->d_name), strlen(result->d_name));
		return sizeof(php_stream_dirent);
	}
	return 0;
}

static int php_plain_files_dirstream_close(php_stream *stream, int close_handle)
{
	return closedir((DIR *)stream->abstract);
}

static int php_plain_files_dirstream_rewind(php_stream *stream, zend_off_t offset, int whence, zend_off_t *newoffs)
{
	rewinddir((DIR *)stream->abstract);
	return 0;
}

static php_stream_ops	php_plain_files_dirstream_ops = {
	NULL, php_plain_files_dirstream_read,
	php_plain_files_dirstream_close, NULL,
	"dir",
	php_plain_files_dirstream_rewind,
	NULL, /* cast */
	NULL, /* stat */
	NULL  /* set_option */
};

static php_stream *php_plain_files_dir_opener(php_stream_wrapper *wrapper, const char *path, const char *mode,
		int options, zend_string **opened_path, php_stream_context *context STREAMS_DC)
{
	DIR *dir = NULL;
	php_stream *stream = NULL;

#ifdef HAVE_GLOB
	if (options & STREAM_USE_GLOB_DIR_OPEN) {
		return php_glob_stream_wrapper.wops->dir_opener(&php_glob_stream_wrapper, path, mode, options, opened_path, context STREAMS_REL_CC);
	}
#endif

	if (((options & STREAM_DISABLE_OPEN_BASEDIR) == 0) && php_check_open_basedir(path)) {
		return NULL;
	}

	dir = VCWD_OPENDIR(path);

#ifdef PHP_WIN32
	if (!dir) {
		php_win32_docref2_from_error(GetLastError(), path, path);
	}

	if (dir && dir->finished) {
		closedir(dir);
		dir = NULL;
	}
#endif
	if (dir) {
		stream = php_stream_alloc(&php_plain_files_dirstream_ops, dir, 0, mode);
		if (stream == NULL)
			closedir(dir);
	}

	return stream;
}
/* }}} */

/* {{{ php_stream_fopen */
PHPAPI php_stream *_php_stream_fopen(const char *filename, const char *mode, zend_string **opened_path, int options STREAMS_DC)
{
	char realpath[MAXPATHLEN];
	int open_flags;
	int fd;
	php_stream *ret;
	int persistent = options & STREAM_OPEN_PERSISTENT;
	char *persistent_id = NULL;

	if (FAILURE == php_stream_parse_fopen_modes(mode, &open_flags)) {
		if (options & REPORT_ERRORS) {
			php_error_docref(NULL, E_WARNING, "`%s' is not a valid mode for fopen", mode);
		}
		return NULL;
	}

	if (options & STREAM_ASSUME_REALPATH) {
		strlcpy(realpath, filename, sizeof(realpath));
	} else {
		if (expand_filepath(filename, realpath) == NULL) {
			return NULL;
		}
	}

	if (persistent) {
		spprintf(&persistent_id, 0, "streams_stdio_%d_%s", open_flags, realpath);
		switch (php_stream_from_persistent_id(persistent_id, &ret)) {
			case PHP_STREAM_PERSISTENT_SUCCESS:
				if (opened_path) {
					//TODO: avoid reallocation???
					*opened_path = zend_string_init(realpath, strlen(realpath), 0);
				}
				/* fall through */

			case PHP_STREAM_PERSISTENT_FAILURE:
				efree(persistent_id);;
				return ret;
		}
	}

	fd = open(realpath, open_flags, 0666);

	if (fd != -1)	{

		if (options & STREAM_OPEN_FOR_INCLUDE) {
			ret = php_stream_fopen_from_fd_int_rel(fd, mode, persistent_id);
		} else {
			ret = php_stream_fopen_from_fd_rel(fd, mode, persistent_id);
		}

		if (ret)	{
			if (opened_path) {
				*opened_path = zend_string_init(realpath, strlen(realpath), 0);
			}
			if (persistent_id) {
				efree(persistent_id);
			}

			/* WIN32 always set ISREG flag */
#ifndef PHP_WIN32
			/* sanity checks for include/require.
			 * We check these after opening the stream, so that we save
			 * on fstat() syscalls */
			if (options & STREAM_OPEN_FOR_INCLUDE) {
				php_stdio_stream_data *self = (php_stdio_stream_data*)ret->abstract;
				int r;

				r = do_fstat(self, 0);
				if ((r == 0 && !S_ISREG(self->sb.st_mode))) {
					if (opened_path) {
						zend_string_release(*opened_path);
						*opened_path = NULL;
					}
					php_stream_close(ret);
					return NULL;
				}
			}
#endif

			return ret;
		}
		close(fd);
	}
	if (persistent_id) {
		efree(persistent_id);
	}
	return NULL;
}
/* }}} */


static php_stream *php_plain_files_stream_opener(php_stream_wrapper *wrapper, const char *path, const char *mode,
		int options, zend_string **opened_path, php_stream_context *context STREAMS_DC)
{
	if (((options & STREAM_DISABLE_OPEN_BASEDIR) == 0) && php_check_open_basedir(path)) {
		return NULL;
	}

	return php_stream_fopen_rel(path, mode, opened_path, options);
}

static int php_plain_files_url_stater(php_stream_wrapper *wrapper, const char *url, int flags, php_stream_statbuf *ssb, php_stream_context *context)
{
	if (strncasecmp(url, "file://", sizeof("file://") - 1) == 0) {
		url += sizeof("file://") - 1;
	}

	if (php_check_open_basedir_ex(url, (flags & PHP_STREAM_URL_STAT_QUIET) ? 0 : 1)) {
		return -1;
	}

#ifdef PHP_WIN32
	if (flags & PHP_STREAM_URL_STAT_LINK) {
		return VCWD_LSTAT(url, &ssb->sb);
	}
#else
# ifdef HAVE_SYMLINK
	if (flags & PHP_STREAM_URL_STAT_LINK) {
		return VCWD_LSTAT(url, &ssb->sb);
	} else
# endif
#endif
		return VCWD_STAT(url, &ssb->sb);
}

static int php_plain_files_unlink(php_stream_wrapper *wrapper, const char *url, int options, php_stream_context *context)
{
	int ret;

	if (strncasecmp(url, "file://", sizeof("file://") - 1) == 0) {
		url += sizeof("file://") - 1;
	}

	if (php_check_open_basedir(url)) {
		return 0;
	}

	ret = VCWD_UNLINK(url);
	if (ret == -1) {
		if (options & REPORT_ERRORS) {
			php_error_docref1(NULL, url, E_WARNING, "%s", strerror(errno));
		}
		return 0;
	}

	/* Clear stat cache (and realpath cache) */
	php_clear_stat_cache(1, NULL, 0);

	return 1;
}

static int php_plain_files_rename(php_stream_wrapper *wrapper, const char *url_from, const char *url_to, int options, php_stream_context *context)
{
	int ret;

	if (!url_from || !url_to) {
		return 0;
	}

#ifdef PHP_WIN32
	if (!php_win32_check_trailing_space(url_from, (int)strlen(url_from))) {
		php_win32_docref2_from_error(ERROR_INVALID_NAME, url_from, url_to);
		return 0;
	}
	if (!php_win32_check_trailing_space(url_to, (int)strlen(url_to))) {
		php_win32_docref2_from_error(ERROR_INVALID_NAME, url_from, url_to);
		return 0;
	}
#endif

	if (strncasecmp(url_from, "file://", sizeof("file://") - 1) == 0) {
		url_from += sizeof("file://") - 1;
	}

	if (strncasecmp(url_to, "file://", sizeof("file://") - 1) == 0) {
		url_to += sizeof("file://") - 1;
	}

	if (php_check_open_basedir(url_from) || php_check_open_basedir(url_to)) {
		return 0;
	}

	ret = VCWD_RENAME(url_from, url_to);

	if (ret == -1) {
#ifndef PHP_WIN32
# ifdef EXDEV
		if (errno == EXDEV) {
			zend_stat_t sb;
			if (php_copy_file(url_from, url_to) == SUCCESS) {
				if (VCWD_STAT(url_from, &sb) == 0) {
#  if !defined(TSRM_WIN32) && !defined(NETWARE)
					if (VCWD_CHMOD(url_to, sb.st_mode)) {
						if (errno == EPERM) {
							php_error_docref2(NULL, url_from, url_to, E_WARNING, "%s", strerror(errno));
							VCWD_UNLINK(url_from);
							return 1;
						}
						php_error_docref2(NULL, url_from, url_to, E_WARNING, "%s", strerror(errno));
						return 0;
					}
					if (VCWD_CHOWN(url_to, sb.st_uid, sb.st_gid)) {
						if (errno == EPERM) {
							php_error_docref2(NULL, url_from, url_to, E_WARNING, "%s", strerror(errno));
							VCWD_UNLINK(url_from);
							return 1;
						}
						php_error_docref2(NULL, url_from, url_to, E_WARNING, "%s", strerror(errno));
						return 0;
					}
#  endif
					VCWD_UNLINK(url_from);
					return 1;
				}
			}
			php_error_docref2(NULL, url_from, url_to, E_WARNING, "%s", strerror(errno));
			return 0;
		}
# endif
#endif

#ifdef PHP_WIN32
		php_win32_docref2_from_error(GetLastError(), url_from, url_to);
#else
		php_error_docref2(NULL, url_from, url_to, E_WARNING, "%s", strerror(errno));
#endif
		return 0;
	}

	/* Clear stat cache (and realpath cache) */
	php_clear_stat_cache(1, NULL, 0);

	return 1;
}

static int php_plain_files_mkdir(php_stream_wrapper *wrapper, const char *dir, int mode, int options, php_stream_context *context)
{
	int ret, recursive = options & PHP_STREAM_MKDIR_RECURSIVE;
	char *p;

	if (strncasecmp(dir, "file://", sizeof("file://") - 1) == 0) {
		dir += sizeof("file://") - 1;
	}

	if (!recursive) {
		ret = php_mkdir(dir, mode);
	} else {
		/* we look for directory separator from the end of string, thus hopefuly reducing our work load */
		char *e;
		zend_stat_t sb;
		int dir_len = (int)strlen(dir);
		int offset = 0;
		char buf[MAXPATHLEN];

		if (!expand_filepath_with_mode(dir, buf, NULL, 0, CWD_EXPAND )) {
			php_error_docref(NULL, E_WARNING, "Invalid path");
			return 0;
		}

		e = buf +  strlen(buf);

		if ((p = memchr(buf, DEFAULT_SLASH, dir_len))) {
			offset = p - buf + 1;
		}

		if (p && dir_len == 1) {
			/* buf == "DEFAULT_SLASH" */
		}
		else {
			/* find a top level directory we need to create */
			while ( (p = strrchr(buf + offset, DEFAULT_SLASH)) || (offset != 1 && (p = strrchr(buf, DEFAULT_SLASH))) ) {
				int n = 0;

				*p = '\0';
				while (p > buf && *(p-1) == DEFAULT_SLASH) {
					++n;
					--p;
					*p = '\0';
				}
				if (VCWD_STAT(buf, &sb) == 0) {
					while (1) {
						*p = DEFAULT_SLASH;
						if (!n) break;
						--n;
						++p;
					}
					break;
				}
			}
		}

		if (p == buf) {
			ret = php_mkdir(dir, mode);
		} else if (!(ret = php_mkdir(buf, mode))) {
			if (!p) {
				p = buf;
			}
			/* create any needed directories if the creation of the 1st directory worked */
			while (++p != e) {
				if (*p == '\0') {
					*p = DEFAULT_SLASH;
					if ((*(p+1) != '\0') &&
						(ret = VCWD_MKDIR(buf, (mode_t)mode)) < 0) {
						if (options & REPORT_ERRORS) {
							php_error_docref(NULL, E_WARNING, "%s", strerror(errno));
						}
						break;
					}
				}
			}
		}
	}
	if (ret < 0) {
		/* Failure */
		return 0;
	} else {
		/* Success */
		return 1;
	}
}

static int php_plain_files_rmdir(php_stream_wrapper *wrapper, const char *url, int options, php_stream_context *context)
{
	if (strncasecmp(url, "file://", sizeof("file://") - 1) == 0) {
		url += sizeof("file://") - 1;
	}

	if (php_check_open_basedir(url)) {
		return 0;
	}

#if PHP_WIN32
	if (!php_win32_check_trailing_space(url, (int)strlen(url))) {
		php_error_docref1(NULL, url, E_WARNING, "%s", strerror(ENOENT));
		return 0;
	}
#endif

	if (VCWD_RMDIR(url) < 0) {
		php_error_docref1(NULL, url, E_WARNING, "%s", strerror(errno));
		return 0;
	}

	/* Clear stat cache (and realpath cache) */
	php_clear_stat_cache(1, NULL, 0);

	return 1;
}

static int php_plain_files_metadata(php_stream_wrapper *wrapper, const char *url, int option, void *value, php_stream_context *context)
{
	struct utimbuf *newtime;
#if !defined(WINDOWS) && !defined(NETWARE)
	uid_t uid;
	gid_t gid;
#endif
	mode_t mode;
	int ret = 0;
#if PHP_WIN32
	int url_len = (int)strlen(url);
#endif

#if PHP_WIN32
	if (!php_win32_check_trailing_space(url, url_len)) {
		php_error_docref1(NULL, url, E_WARNING, "%s", strerror(ENOENT));
		return 0;
	}
#endif

	if (strncasecmp(url, "file://", sizeof("file://") - 1) == 0) {
		url += sizeof("file://") - 1;
	}

	if (php_check_open_basedir(url)) {
		return 0;
	}

	switch(option) {
		case PHP_STREAM_META_TOUCH:
			newtime = (struct utimbuf *)value;
			if (VCWD_ACCESS(url, F_OK) != 0) {
				FILE *file = VCWD_FOPEN(url, "w");
				if (file == NULL) {
					php_error_docref1(NULL, url, E_WARNING, "Unable to create file %s because %s", url, strerror(errno));
					return 0;
				}
				fclose(file);
			}

			ret = VCWD_UTIME(url, newtime);
			break;
#if !defined(WINDOWS) && !defined(NETWARE)
		case PHP_STREAM_META_OWNER_NAME:
		case PHP_STREAM_META_OWNER:
			if(option == PHP_STREAM_META_OWNER_NAME) {
				if(php_get_uid_by_name((char *)value, &uid) != SUCCESS) {
					php_error_docref1(NULL, url, E_WARNING, "Unable to find uid for %s", (char *)value);
					return 0;
				}
			} else {
				uid = (uid_t)*(long *)value;
			}
			ret = VCWD_CHOWN(url, uid, -1);
			break;
		case PHP_STREAM_META_GROUP:
		case PHP_STREAM_META_GROUP_NAME:
			if(option == PHP_STREAM_META_GROUP_NAME) {
				if(php_get_gid_by_name((char *)value, &gid) != SUCCESS) {
					php_error_docref1(NULL, url, E_WARNING, "Unable to find gid for %s", (char *)value);
					return 0;
				}
			} else {
				gid = (gid_t)*(long *)value;
			}
			ret = VCWD_CHOWN(url, -1, gid);
			break;
#endif
		case PHP_STREAM_META_ACCESS:
			mode = (mode_t)*(zend_long *)value;
			ret = VCWD_CHMOD(url, mode);
			break;
		default:
			php_error_docref1(NULL, url, E_WARNING, "Unknown option %d for stream_metadata", option);
			return 0;
	}
	if (ret == -1) {
		php_error_docref1(NULL, url, E_WARNING, "Operation failed: %s", strerror(errno));
		return 0;
	}
	php_clear_stat_cache(0, NULL, 0);
	return 1;
}


static php_stream_wrapper_ops php_plain_files_wrapper_ops = {
	php_plain_files_stream_opener,
	NULL,
	NULL,
	php_plain_files_url_stater,
	php_plain_files_dir_opener,
	"plainfile",
	php_plain_files_unlink,
	php_plain_files_rename,
	php_plain_files_mkdir,
	php_plain_files_rmdir,
	php_plain_files_metadata
};

PHPAPI php_stream_wrapper php_plain_files_wrapper = {
	&php_plain_files_wrapper_ops,
	NULL,
	0
};

/* {{{ php_stream_fopen_with_path */
PHPAPI php_stream *_php_stream_fopen_with_path(const char *filename, const char *mode, const char *path, zend_string **opened_path, int options STREAMS_DC)
{
	/* code ripped off from fopen_wrappers.c */
	char *pathbuf, *end;
	const char *ptr;
	char trypath[MAXPATHLEN];
	php_stream *stream;
	int filename_length;
	zend_string *exec_filename;

	if (opened_path) {
		*opened_path = NULL;
	}

	if(!filename) {
		return NULL;
	}

	filename_length = (int)strlen(filename);
#ifndef PHP_WIN32
	(void) filename_length;
#endif

	/* Relative path open */
	if (*filename == '.' && (IS_SLASH(filename[1]) || filename[1] == '.')) {
		/* further checks, we could have ....... filenames */
		ptr = filename + 1;
		if (*ptr == '.') {
			while (*(++ptr) == '.');
			if (!IS_SLASH(*ptr)) { /* not a relative path after all */
				goto not_relative_path;
			}
		}

		if (((options & STREAM_DISABLE_OPEN_BASEDIR) == 0) && php_check_open_basedir(filename)) {
			return NULL;
		}

		return php_stream_fopen_rel(filename, mode, opened_path, options);
	}

not_relative_path:

	/* Absolute path open */
	if (IS_ABSOLUTE_PATH(filename, filename_length)) {

		if (((options & STREAM_DISABLE_OPEN_BASEDIR) == 0) && php_check_open_basedir(filename)) {
			return NULL;
		}

		return php_stream_fopen_rel(filename, mode, opened_path, options);
	}

#ifdef PHP_WIN32
	if (IS_SLASH(filename[0])) {
		size_t cwd_len;
		char *cwd;
		cwd = virtual_getcwd_ex(&cwd_len);
		/* getcwd() will return always return [DRIVE_LETTER]:/) on windows. */
		*(cwd+3) = '\0';

		if (snprintf(trypath, MAXPATHLEN, "%s%s", cwd, filename) >= MAXPATHLEN) {
			php_error_docref(NULL, E_NOTICE, "%s/%s path was truncated to %d", cwd, filename, MAXPATHLEN);
		}

		efree(cwd);

		if (((options & STREAM_DISABLE_OPEN_BASEDIR) == 0) && php_check_open_basedir(trypath)) {
			return NULL;
		}

		return php_stream_fopen_rel(trypath, mode, opened_path, options);
	}
#endif

	if (!path || (path && !*path)) {
		return php_stream_fopen_rel(filename, mode, opened_path, options);
	}

	/* check in provided path */
	/* append the calling scripts' current working directory
	 * as a fall back case
	 */
	if (zend_is_executing() &&
	    (exec_filename = zend_get_executed_filename_ex()) != NULL) {
		const char *exec_fname = exec_filename->val;
		size_t exec_fname_length = exec_filename->len;

		while ((--exec_fname_length < SIZE_MAX) && !IS_SLASH(exec_fname[exec_fname_length]));
		if (exec_fname_length<=0) {
			/* no path */
			pathbuf = estrdup(path);
		} else {
			size_t path_length = strlen(path);

			pathbuf = (char *) emalloc(exec_fname_length + path_length +1 +1);
			memcpy(pathbuf, path, path_length);
			pathbuf[path_length] = DEFAULT_DIR_SEPARATOR;
			memcpy(pathbuf+path_length+1, exec_fname, exec_fname_length);
			pathbuf[path_length + exec_fname_length +1] = '\0';
		}
	} else {
		pathbuf = estrdup(path);
	}

	ptr = pathbuf;

	while (ptr && *ptr) {
		end = strchr(ptr, DEFAULT_DIR_SEPARATOR);
		if (end != NULL) {
			*end = '\0';
			end++;
		}
		if (*ptr == '\0') {
			goto stream_skip;
		}
		if (snprintf(trypath, MAXPATHLEN, "%s/%s", ptr, filename) >= MAXPATHLEN) {
			php_error_docref(NULL, E_NOTICE, "%s/%s path was truncated to %d", ptr, filename, MAXPATHLEN);
		}

		if (((options & STREAM_DISABLE_OPEN_BASEDIR) == 0) && php_check_open_basedir_ex(trypath, 0)) {
			goto stream_skip;
		}

		stream = php_stream_fopen_rel(trypath, mode, opened_path, options);
		if (stream) {
			efree(pathbuf);
			return stream;
		}
stream_skip:
		ptr = end;
	} /* end provided path */

	efree(pathbuf);
	return NULL;

}
/* }}} */

/* {{{ php_stream_freopen */
PHPAPI int _php_stream_freopen(char *filename, char *mode, php_stream *stream STREAMS_DC TSRMLS_DC)
{
	int open_flags;
	char *realpath = NULL;
	int fd;

	int fileno = ((php_stdio_stream_data *)stream->abstract)->fd;

	if (!filename || !*filename) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Filename cannot be empty");
		return 0;
	}

	if (FAILURE == php_stream_parse_fopen_modes(mode, &open_flags)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "`%s' is not a valid mode for freopen", mode);
		return 0;
	}

	if ((realpath = expand_filepath(filename, NULL TSRMLS_CC)) == NULL) {
		return 0;
	}

	if (php_stream_flush(stream)) {
		efree(realpath);
		return 0;
	}

	fd = open(realpath, open_flags, 0666);
	efree(realpath);

	if (fd == -1) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", strerror(errno));
		return 0;
	}

	if (dup2(fd, fileno) == -1) {
		close(fd);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", strerror(errno));
		return 0;
	}
	close(fd);

	pefree(stream->orig_path, stream->is_persistent);
	stream->orig_path = pestrdup(filename, stream->is_persistent);
	return 1;
}
/* }}} */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
