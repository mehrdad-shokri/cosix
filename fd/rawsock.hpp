#pragma once
#include <fd/sock.hpp>
#include <fd/process_fd.hpp>
#include <oslibc/list.hpp>
#include <net/interface.hpp>

namespace cloudos {

struct rawsock : public sock_t, public enable_shared_from_this<rawsock> {
	rawsock(interface *iface, const char *n);
	~rawsock() override;

	void init();

	void sock_shutdown(cloudabi_sdflags_t how) override;
	void sock_stat_get(cloudabi_sockstat_t* buf, cloudabi_ssflags_t flags) override;
	void sock_recv(const cloudabi_recv_in_t* in, cloudabi_recv_out_t *out) override;
	void sock_send(const cloudabi_send_in_t* in, cloudabi_send_out_t *out) override;

	void interface_recv(uint8_t *frame, size_t frame_length, protocol_t frame_type, size_t ip_hdr_offset);

private:
	// TODO: make this a weak ptr
	interface *iface;

	linked_list<Blk> *messages = nullptr;
	cv_t read_cv;
};

}