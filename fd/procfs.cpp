#include "procfs.hpp"
#include "global.hpp"
#include <oslibc/numeric.h>
#include <memory/allocator.hpp>

using namespace cloudos;

static const int PROCFS_DEPTH_MAX = 5;
static const int PROCFS_FILE_MAX = 40;

namespace cloudos {

struct procfs_directory_fd : fd_t {
	procfs_directory_fd(const char (*)[PROCFS_FILE_MAX], const char *n);

	error_t to_string(char *buf, size_t length);

	fd_t *openat(const char *pathname, bool directory) override;

private:
	char path[PROCFS_DEPTH_MAX][PROCFS_FILE_MAX];
	size_t depth;
};

struct procfs_uptime_fd : fd_t {
	procfs_uptime_fd(const char *n) : fd_t(CLOUDABI_FILETYPE_REGULAR_FILE, n) {}

	size_t read(size_t offset, void *dest, size_t count) override;
};

}

procfs_directory_fd::procfs_directory_fd(const char (*p)[PROCFS_FILE_MAX], const char *n)
: fd_t(CLOUDABI_FILETYPE_DIRECTORY, n)
{
	for(depth = 0; depth < PROCFS_DEPTH_MAX; ++depth) {
		if(p[depth] && p[depth][0]) {
			strncpy(path[depth], p[depth], PROCFS_FILE_MAX);
		} else {
			break;
		}
	}
	if(depth == PROCFS_DEPTH_MAX) {
		kernel_panic("procfs_directory_fd too deep, but already in constructor");
	}
	for(size_t i = depth; i < PROCFS_DEPTH_MAX; ++i) {
		path[i][0] = 0;
	}

	char pathbuf[PROCFS_DEPTH_MAX * (PROCFS_FILE_MAX + 1)];
	if(to_string(pathbuf, sizeof(pathbuf)) == error_t::no_error) {
		get_vga_stream() << "A procfs_directory_fd was created with path: " << pathbuf << "\n";
	} else {
		get_vga_stream() << "A procfs_directory_fd was created but path didn't fit?\n";
	}
}

error_t procfs_directory_fd::to_string(char *buf, size_t length) {
	size_t bytes_written = 0;
	buf[0] = 0;
	for(size_t i = 0; i < depth; ++i) {
		size_t elemlen = strlen(path[i]); // excluding the ending nullbyte
		size_t length_left = length - bytes_written; // including the ending nullbyte
		if(elemlen >= length_left || (i != depth && elemlen >= length_left - 1)) {
			// no memory to store this or the next path item
			strncpy(&buf[bytes_written], path[i], length_left - 1);
			buf[length] = 0;
			return error_t::no_memory;
		}
		strncpy(&buf[bytes_written], path[i], elemlen);
		bytes_written += elemlen;
		if(i != depth) {
			// a path follows
			buf[bytes_written] = '/';
			++bytes_written;
		}
		buf[bytes_written] = 0;
	}
	return error_t::no_error;
}

fd_t *procfs_directory_fd::openat(const char *pathname, bool directory) {
	if(pathname == 0 || pathname[0] == 0 || pathname[0] == '/') {
		error = error_t::invalid_argument;
		return nullptr;
	}

	size_t path_length = strlen(pathname);

	char pathbuf[PROCFS_DEPTH_MAX * (PROCFS_FILE_MAX + 1)];
	if(to_string(pathbuf, sizeof(pathbuf)) != error_t::no_error) {
		kernel_panic("to_string() should always succeed");
	}
	size_t pathbuf_length = strlen(pathbuf);

	if(path_length + 1 + pathbuf_length >= sizeof(pathbuf)) {
		error = error_t::no_memory;
		return nullptr;
	}

	// TODO: actual path resolving with './', '../', symlinks and hardlinks
	strncpy(&pathbuf[pathbuf_length], pathname, path_length + 1);

	if(strcmp(pathbuf, "kernel/uptime") == 0) {
		if(directory) {
			error = error_t::not_a_directory;
			return nullptr;
		} else {
			error = error_t::no_error;
			procfs_uptime_fd *fd = get_allocator()->allocate<procfs_uptime_fd>();
			new (fd) procfs_uptime_fd(pathbuf);
			return fd;
		}
	} else if(strcmp(pathbuf, "kernel") == 0 || strcmp(pathbuf, "kernel/") == 0) {
		// directories can be opened without O_DIRECTORY as well
		error = error_t::no_error;
		char pb[2][PROCFS_FILE_MAX];
		strncpy(pb[0], "kernel", PROCFS_FILE_MAX);
		pb[1][0] = 0;
		procfs_directory_fd *fd = get_allocator()->allocate<procfs_directory_fd>();
		new (fd) procfs_directory_fd(pb, "procfs_kernel_dir");
		return fd;
	} else {
		error = error_t::no_entity;
		return nullptr;
	}
}

size_t procfs_uptime_fd::read(size_t offset, void *dest, size_t count) {
	error = error_t::no_error;
	// TODO compute uptime
	int uptime = 12345;
	char buf[10];

	// TODO this is the same code for every file for which we
	// already have all contents in memory, so unify this
	char *addr = ui64toa_s(uptime, buf, sizeof(buf), 10);
	size_t length = strlen(addr);
	if(offset + count > length) {
		reinterpret_cast<char*>(dest)[0] = 0;
		return 0;
	}

	size_t bytes_left = length - offset;
	size_t copied = count < bytes_left ? count : bytes_left;
	memcpy(reinterpret_cast<char*>(dest), addr + offset, copied);
	return copied;
}

fd_t *procfs::get_root_fd() {
	procfs_directory_fd *fd = get_allocator()->allocate<procfs_directory_fd>();
	new (fd) procfs_directory_fd(NULL, "procfs_root");
	return fd;
}