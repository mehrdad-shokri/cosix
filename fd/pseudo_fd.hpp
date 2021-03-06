#pragma once

#include "reverse_fd.hpp"
#include "fd.hpp"
#include "reverse_proto.hpp"

namespace cloudos {

using reverse_proto::reverse_request_t;
using reverse_proto::reverse_response_t;
using reverse_proto::pseudofd_t;

/** A pseudo-fd. This is a file descriptor where all calls on it are converted
 * to RPCs. These RPCs are sent to a given file descriptor, called the "reverse
 * fd". This is usually a socket with the other end given to a process, so that
 * the process can handle and respond to all calls on its pseudo fd's. It is
 * given to the constructor of the pseudo_fd. Only one request can be
 * outstanding on a reverse fd.
 *
 * The other side of the reverse FD will create a new pseudo FDs in the open
 * call.
 */
struct pseudo_fd : public seekable_fd_t, public enable_shared_from_this<pseudo_fd> {
	pseudo_fd(pseudofd_t id, shared_ptr<reversefd_t> reverse_fd, cloudabi_filetype_t t, cloudabi_fdflags_t f, const char *n);
	~pseudo_fd() override;

	inline pseudofd_t get_pseudo_id() { return pseudo_id; }
	inline shared_ptr<reversefd_t> get_reverse_fd() { return reverse_fd; }

	/* For memory, pipes and files */
	size_t read(void *dest, size_t count) override;
	size_t write(const char *str, size_t count) override;
	bool is_readable(size_t &nbytes, bool &hangup);
	cloudabi_errno_t get_read_signaler(thread_condition_signaler **s) override;
	void became_readable();
	void datasync() override;
	void sync() override;

	/* For directories */
	void lookup(const char *file, size_t filelen, cloudabi_oflags_t oflags, cloudabi_filestat_t *filestat) override;
	shared_ptr<fd_t> inode_open(cloudabi_device_t st_dev, cloudabi_inode_t st_ino, const cloudabi_fdstat_t *) override;
	void file_allocate(cloudabi_filesize_t offset, cloudabi_filesize_t length) override;
	size_t readdir(char *buf, size_t nbyte, cloudabi_dircookie_t cookie) override;
	cloudabi_inode_t file_create(const char *file, size_t filelen, cloudabi_filetype_t type) override;
	size_t file_readlink(const char *path, size_t pathlen, char *buf, size_t buflen) override;
	void file_rename(const char *path1, size_t path1len, shared_ptr<fd_t> fd2, const char *path2, size_t path2len) override;
	void file_symlink(const char *path1, size_t path1len, const char *path2, size_t path2len) override;
	void file_link(const char *path1, size_t path1len, shared_ptr<fd_t> fd2, const char *path2, size_t path2len) override;
	void file_unlink(const char *path, size_t pathlen, cloudabi_ulflags_t flags) override;
	void file_stat_fget(cloudabi_filestat_t *buf) override;
	void file_stat_put(const char *path, size_t pathlen, const cloudabi_filestat_t *buf, cloudabi_fsflags_t fsflags) override;
	void file_stat_fput(const cloudabi_filestat_t *buf, cloudabi_fsflags_t fsflags) override;

	/* For sockets */
	void sock_shutdown(cloudabi_sdflags_t how) override;
	void sock_recv(const cloudabi_recv_in_t* in, cloudabi_recv_out_t* out) override;
	void sock_send(const cloudabi_send_in_t* in, cloudabi_send_out_t* out) override;

private:
	cloudabi_errno_t lookup_device_id();
	Blk send_request(reverse_request_t *request, const char *buf, reverse_response_t *response);
	bool is_valid_path(const char *path, size_t length);

	pseudofd_t pseudo_id;
	shared_ptr<reversefd_t> reverse_fd;
	bool device_id_obtained = false;
	thread_condition_signaler recv_signaler;
};

}
