#pragma once

#include "fd.hpp"
#include "pipe_fd.hpp"

namespace cloudos {

/**
 * A socket FD.
 *
 * This FD consists of a pipe (fifo) FD to read from, and one to write to.
 * This way, it implements bidirectional communication using the FIFO building
 * block. Use the socketpair() factory function to build a complete socket
 * pair from scratch.
 */
struct socket_fd : fd_t {
	socket_fd(shared_ptr<pipe_fd> read, shared_ptr<pipe_fd> write, const char *n);

	/** read() blocks until at least 1 byte of data is available;
	 * then, it returns up to count bytes of data in the dest buffer.
	 * It sets invalid_argument as the error if offset is not 0.
	 */
	size_t read(void *dest, size_t count) override;

	/** putstring() blocks until there is capacity for at least count
	 * bytes, then, it appends the given buffer to the stored one.
	 */
	void putstring(const char * /*str*/, size_t /*count*/) override;

	/** Creates two socket_fd's that form a connected pair. */
	static void socketpair(shared_ptr<socket_fd> &a, shared_ptr<socket_fd> &b, size_t capacity);

private:
	shared_ptr<pipe_fd> readfd;
	shared_ptr<pipe_fd> writefd;
};

}
