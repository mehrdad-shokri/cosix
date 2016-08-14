#include "process_fd.hpp"
#include <oslibc/string.h>
#include <hw/vga_stream.hpp>
#include <net/elfrun.hpp> /* for elf_endian */
#include <memory/allocator.hpp>
#include <memory/page_allocator.hpp>
#include <global.hpp>
#include <fd/vga_fd.hpp>
#include <fd/memory_fd.hpp>
#include <fd/procfs.hpp>
#include <fd/bootfs.hpp>
#include <userland/vdso_support.h>

extern uint32_t _kernel_virtual_base;

using namespace cloudos;

process_fd::process_fd(page_allocator *a, const char *n)
: fd_t(CLOUDABI_FILETYPE_PROCESS, n)
{
	page_allocation p;
	auto res = a->allocate(&p);
	if(res != error_t::no_error) {
		kernel_panic("Failed to allocate process paging directory");
	}
	page_directory = reinterpret_cast<uint32_t*>(p.address);
	memset(page_directory, 0, PAGE_DIRECTORY_SIZE * sizeof(uint32_t));
	a->fill_kernel_pages(page_directory);

	res = a->allocate(&p);
	if(res != error_t::no_error) {
		kernel_panic("Failed to allocate page table list");
	}
	page_tables = reinterpret_cast<uint32_t**>(p.address);
	for(size_t i = 0; i < 0x300; ++i) {
		page_tables[i] = nullptr;
	}

	vga_fd *fd = get_allocator()->allocate<vga_fd>();
	new (fd) vga_fd("vga_fd");
	add_fd(fd, CLOUDABI_RIGHT_FD_WRITE);

	char *fd_buf = get_allocator()->allocate<char>(200);
	strncpy(fd_buf, "These are the contents of my buffer!\n", 200);

	memory_fd *fd2 = get_allocator()->allocate<memory_fd>();
	new (fd2) memory_fd(fd_buf, strlen(fd_buf) + 1, "memory_fd");
	add_fd(fd2, CLOUDABI_RIGHT_FD_READ);

	add_fd(procfs::get_root_fd(), CLOUDABI_RIGHT_FILE_OPEN, CLOUDABI_RIGHT_FD_READ | CLOUDABI_RIGHT_FILE_OPEN);

	add_fd(bootfs::get_root_fd(), CLOUDABI_RIGHT_FILE_OPEN, CLOUDABI_RIGHT_FD_READ | CLOUDABI_RIGHT_FILE_OPEN);
}

int process_fd::add_fd(fd_t *fd, cloudabi_rights_t rights_base, cloudabi_rights_t rights_inheriting) {
	if(last_fd >= MAX_FD - 1) {
		// TODO: instead of keeping a last_fd counter, put mappings
		// into a freelist when they are closed, and allow them to be
		// reused. Then, return an error when there is no more free
		// space for fd's.
		kernel_panic("fd's expired for process");
	}
	fd_mapping_t *mapping = get_allocator()->allocate<fd_mapping_t>();
	mapping->fd = fd;
	mapping->rights_base = rights_base;
	mapping->rights_inheriting = rights_inheriting;

	int fdnum = ++last_fd;
	fds[fdnum] = mapping;
	return fdnum;
}

error_t process_fd::get_fd(fd_mapping_t **r_mapping, size_t num, cloudabi_rights_t has_rights) {
	*r_mapping = 0;
	if(num >= MAX_FD) {
		get_vga_stream() << "fdnum " << num << " is too high for an fd\n";
		return error_t::resource_exhausted;
	}
	fd_mapping_t *mapping = fds[num];
	if(mapping == 0 || mapping->fd == 0) {
		get_vga_stream() << "fdnum " << num << " is not a valid fd\n";
		return error_t::invalid_argument;
	}
	if((mapping->rights_base & has_rights) != has_rights) {
		get_vga_stream() << "get_fd: fd " << num << " has insufficient rights 0x" << hex << has_rights << dec << "\n";
		return error_t::not_capable;
	}
	*r_mapping = mapping;
	return error_t::no_error;
}

template <typename T>
static inline T *allocate_on_stack(uint32_t *&stack_addr, uint32_t &useresp) {
	stack_addr = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(stack_addr) - sizeof(T));
	useresp -= sizeof(T);
	return reinterpret_cast<T*>(stack_addr);
}

void cloudos::process_fd::initialize(void *start_addr, cloudos::allocator *alloc) {
	userland_stack_size = kernel_stack_size = 0x10000 /* 64 kb */;
	userland_stack_bottom = reinterpret_cast<uint8_t*>(alloc->allocate_aligned(userland_stack_size, 4096));
	kernel_stack_bottom   = reinterpret_cast<uint8_t*>(alloc->allocate_aligned(kernel_stack_size, 4096));

	// initialize all registers and return state to zero
	memset(&state, 0, sizeof(state));

	// set ring 3 data, stack & code segments
	state.ds = state.ss = 0x23;
	state.cs = 0x1b;
	// set fsbase
	state.fs = 0x33;

	// stack location for new process
	uint32_t *stack_addr = reinterpret_cast<uint32_t*>(reinterpret_cast<uint32_t>(userland_stack_bottom) + userland_stack_size);
	userland_stack_address = reinterpret_cast<void*>(0x80000000);
	map_at(userland_stack_bottom, reinterpret_cast<void*>(reinterpret_cast<uint32_t>(userland_stack_address) - userland_stack_size), userland_stack_size);
	state.useresp = reinterpret_cast<uint32_t>(userland_stack_address);

	// initialize vdso address
	vdso_size = vdso_blob_size;
	vdso_image = alloc->allocate_aligned(vdso_size, 4096);
	memcpy(vdso_image, vdso_blob, vdso_size);
	uint32_t vdso_address = 0x80040000;
	map_at(vdso_image, reinterpret_cast<void*>(vdso_address), vdso_size);

	// initialize elf phdr address
	uint32_t elf_phdr_address = 0x80060000;
	map_at(elf_phdr, reinterpret_cast<void*>(elf_phdr_address), elf_ph_size);

	// initialize auxv
	if(elf_ph_size == 0 || elf_phdr == 0) {
		kernel_panic("About to start process but no elf_phdr present");
	}
	size_t auxv_entries = 6; // including CLOUDABI_AT_NULL
	auxv_size = auxv_entries * sizeof(cloudabi_auxv_t);
	auxv_buf = alloc->allocate_aligned(auxv_size, 4096);
	cloudabi_auxv_t *auxv = reinterpret_cast<cloudabi_auxv_t*>(auxv_buf);
	auxv->a_type = CLOUDABI_AT_BASE;
	auxv->a_ptr = nullptr; /* because we don't do address randomization */
	auxv++;
	auxv->a_type = CLOUDABI_AT_PAGESZ;
	auxv->a_val = PAGE_SIZE;
	auxv++;
	auxv->a_type = CLOUDABI_AT_SYSINFO_EHDR;
	auxv->a_val = vdso_address;
	auxv++;
	auxv->a_type = CLOUDABI_AT_PHDR;
	auxv->a_val = elf_phdr_address;
	auxv++;
	auxv->a_type = CLOUDABI_AT_PHNUM;
	auxv->a_val = elf_phnum;
	auxv++;
	auxv->a_type = CLOUDABI_AT_NULL;
	uint32_t auxv_address = 0x80010000;
	map_at(auxv_buf, reinterpret_cast<void*>(auxv_address), auxv_size);

	// memory for the TCB pointer and area
	void **tcb_address = allocate_on_stack<void*>(stack_addr, state.useresp);
	cloudabi_tcb_t *tcb = allocate_on_stack<cloudabi_tcb_t>(stack_addr, state.useresp);
	*tcb_address = reinterpret_cast<void*>(state.useresp);
	// we don't currently use the TCB pointer, so set it to zero
	memset(tcb, 0, sizeof(*tcb));

	// initialize stack so that it looks like _start(auxv_address) is called
	*allocate_on_stack<void*>(stack_addr, state.useresp) = reinterpret_cast<void*>(auxv_address);
	*allocate_on_stack<void*>(stack_addr, state.useresp) = 0;

	// initial instruction pointer
	state.eip = reinterpret_cast<uint32_t>(start_addr);

	// allow interrupts
	const int INTERRUPT_ENABLE = 1 << 9;
	state.eflags = INTERRUPT_ENABLE;
}

void cloudos::process_fd::set_return_state(interrupt_state_t *new_state) {
	state = *new_state;
}

void cloudos::process_fd::get_return_state(interrupt_state_t *return_state) {
	*return_state = state;
}

void cloudos::process_fd::handle_syscall(vga_stream &stream) {
	// software interrupt
	int syscall = state.eax;
	if(syscall == 1) {
		// getpid(), returns eax=pid
		state.eax = pid;
	} else if(syscall == 2) {
		// putstring(ebx=fd, ecx=ptr, edx=size), returns eax=0 or eax=-1 on error
		int fdnum = state.ebx;
		fd_mapping_t *mapping;
		auto res = get_fd(&mapping, fdnum, CLOUDABI_RIGHT_FD_WRITE);
		if(res != error_t::no_error) {
			state.eax = -1;
			return;
		}

		const char *str = reinterpret_cast<const char*>(state.ecx);
		const size_t size = state.edx;

		if(reinterpret_cast<uint32_t>(str) >= _kernel_virtual_base
		|| reinterpret_cast<uint32_t>(str) + size >= _kernel_virtual_base
		|| size >= 0x40000000
		|| get_page_allocator()->to_physical_address(this, reinterpret_cast<const void*>(str)) == nullptr) {
			get_vga_stream() << "putstring() of a non-userland-accessible string\n";
			state.eax = -1;
			return;
		}

		res = mapping->fd->putstring(str, size);
		state.eax = res == error_t::no_error ? 0 : -1;
	} else if(syscall == 3) {
		// getchar(ebx=fd, ecx=offset), returns eax=resultchar or eax=-1 on error
		int fdnum = state.ebx;
		fd_mapping_t *mapping;
		auto res = get_fd(&mapping, fdnum, CLOUDABI_RIGHT_FD_READ);
		if(res != error_t::no_error) {
			state.eax = -1;
			return;
		}

		size_t offset = state.ecx;
		char buf[1];

		size_t r = mapping->fd->read(offset, &buf[0], 1);
		if(r != 1 || mapping->fd->error != error_t::no_error) {
			state.eax = -1;
			return;
		}
		state.eax = buf[0];
	} else if(syscall == 4) {
		// openat(ebx=fd, ecx=pathname, edx=as_directory) returns eax=fd or eax=-1 on error
		int fdnum = state.ebx;
		fd_mapping_t *mapping;
		auto res = get_fd(&mapping, fdnum, CLOUDABI_RIGHT_FILE_OPEN);
		if(res != error_t::no_error) {
			state.eax = -1;
			return;
		}

		const char *pathname = reinterpret_cast<const char*>(state.ecx);
		int directory = state.edx;

		fd_t *new_fd = mapping->fd->openat(pathname, directory == 1);
		if(!new_fd || mapping->fd->error != error_t::no_error) {
			get_vga_stream() << "failed to openat()\n";
			state.eax = -1;
			return;
		}

		int new_fdnum = add_fd(new_fd, mapping->rights_inheriting, mapping->rights_inheriting);
		state.eax = new_fdnum;
	} else if(syscall == 5) {
		// sys_fd_stat_get(ebx=fd, ecx=fdstat_t) returns eax=fd or eax=-1 on error
		int fdnum = state.ebx;
		fd_mapping_t *mapping;
		auto res = get_fd(&mapping, fdnum, 0);
		if(res != error_t::no_error) {
			state.eax = -1;
			return;
		}

		cloudabi_fdstat_t *stat = reinterpret_cast<cloudabi_fdstat_t*>(state.ecx);

		// TODO: check if ecx until ecx+sizeof(fdstat_t) is valid *writable* process memory
		if(reinterpret_cast<uint32_t>(stat) >= _kernel_virtual_base
		|| reinterpret_cast<uint32_t>(stat) + sizeof(cloudabi_fdstat_t) >= _kernel_virtual_base
		|| get_page_allocator()->to_physical_address(this, reinterpret_cast<const void*>(stat)) == nullptr) {
			get_vga_stream() << "sys_fd_stat_get() of a non-userland-accessible string\n";
			state.eax = -1;
			return;
		}

		stat->fs_filetype = mapping->fd->type;
		stat->fs_flags = mapping->fd->flags;
		stat->fs_rights_base = mapping->rights_base;
		stat->fs_rights_inheriting = mapping->rights_inheriting;
		state.eax = 0;
	} else {
		stream << "Syscall " << state.eax << " unknown\n";
	}
}

void *cloudos::process_fd::get_kernel_stack_top() {
	return reinterpret_cast<char*>(kernel_stack_bottom) + kernel_stack_size;
}

void *cloudos::process_fd::get_fsbase() {
	return reinterpret_cast<void*>(reinterpret_cast<uint32_t>(userland_stack_address) - sizeof(void*));
}

uint32_t *process_fd::get_page_table(int i) {
	if(i >= 0x300) {
		kernel_panic("process_fd::get_page_table() cannot answer for kernel pages");
	}
	if(page_directory[i] & 0x1 /* present */) {
		return page_tables[i];
	} else {
		return nullptr;
	}
}

void process_fd::install_page_directory() {
	/* some sanity checks to warn early if the page directory looks incorrect */
	if(get_page_allocator()->to_physical_address(this, reinterpret_cast<void*>(0xc00b8000)) != reinterpret_cast<void*>(0xb8000)) {
		kernel_panic("Failed to map VGA page, VGA stream will fail later");
	}
	if(get_page_allocator()->to_physical_address(this, reinterpret_cast<void*>(0xc01031c6)) != reinterpret_cast<void*>(0x1031c6)) {
		kernel_panic("Kernel will fail to execute");
	}

#ifndef TESTING_ENABLED
	auto page_phys_address = get_page_allocator()->to_physical_address(&page_directory[0]);
	if((reinterpret_cast<uint32_t>(page_phys_address) & 0xfff) != 0) {
		kernel_panic("Physically allocated memory is not page-aligned");
	}
	// Set the paging directory in cr3
	asm volatile("mov %0, %%cr3" : : "a"(reinterpret_cast<uint32_t>(page_phys_address)) : "memory");

	// Turn on paging in cr0
	int cr0;
	asm volatile("mov %%cr0, %0" : "=a"(cr0));
	cr0 |= 0x80000000;
	asm volatile("mov %0, %%cr0" : : "a"(cr0) : "memory");
#endif
}

void process_fd::map_at(void *kernel_virt, void *userland_virt, size_t length)
{
	while(true) {
		if(reinterpret_cast<uint32_t>(kernel_virt) < _kernel_virtual_base) {
			kernel_panic("Got non-kernel address in map_at()");
		}
		if(reinterpret_cast<uint32_t>(userland_virt) + length > _kernel_virtual_base) {
			kernel_panic("Got kernel address in map_at()");
		}
		if(reinterpret_cast<uint32_t>(kernel_virt) % PAGE_SIZE != 0) {
			kernel_panic("kernel_virt is not page aligned in map_at");
		}
		if(reinterpret_cast<uint32_t>(userland_virt) % PAGE_SIZE != 0) {
			kernel_panic("userland_virt is not page aligned in map_at");
		}

		void *phys_addr = get_page_allocator()->to_physical_address(kernel_virt);
		if(reinterpret_cast<uint32_t>(phys_addr) % PAGE_SIZE != 0) {
			kernel_panic("phys_addr is not page aligned in map_at");
		}

		uint16_t page_table_num = reinterpret_cast<uint64_t>(userland_virt) >> 22;
		if((page_directory[page_table_num] & 0x1) == 0) {
			// allocate page table
			page_allocation p;
			auto res = get_page_allocator()->allocate(&p);
			if(res != error_t::no_error) {
				kernel_panic("Failed to allocate kernel paging table in map_to");
			}

			auto address = get_page_allocator()->to_physical_address(p.address);
			if((reinterpret_cast<uint32_t>(address) & 0xfff) != 0) {
				kernel_panic("physically allocated memory is not page-aligned");
			}

			page_directory[page_table_num] = reinterpret_cast<uint64_t>(address) | 0x07 /* read-write userspace-accessible present table */;
			page_tables[page_table_num] = reinterpret_cast<uint32_t*>(p.address);
		}
		uint32_t *page_table = get_page_table(page_table_num);
		if(page_table == 0) {
			kernel_panic("Failed to map page table in map_to");
		}

		uint16_t page_entry_num = reinterpret_cast<uint64_t>(userland_virt) >> 12 & 0x03ff;
		uint32_t &page_entry = page_table[page_entry_num];
		if(page_entry & 0x1) {
			get_vga_stream() << "Page table " << page_table_num << ", page entry " << page_entry_num << " already mapped\n";
			get_vga_stream() << "Value: 0x" << hex << page_entry << dec << "\n";
			kernel_panic("Page in map_to already present");
		} else {
			page_entry = reinterpret_cast<uint32_t>(phys_addr) | 0x07; // read-write userspace-accessible present entry
		}

		if(length <= PAGE_SIZE) {
			break;
		}

		kernel_virt = reinterpret_cast<void*>(reinterpret_cast<uint32_t>(kernel_virt) + PAGE_SIZE);
		userland_virt = reinterpret_cast<void*>(reinterpret_cast<uint32_t>(userland_virt) + PAGE_SIZE);
		length -= PAGE_SIZE;
	}
}

void process_fd::copy_and_map_elf(uint8_t *buffer, size_t size)
{
	if(size < 0x30) {
		kernel_panic("copy_and_map_elf has a buffer too small for the basic elf header");
	}

	uint8_t elf_data = buffer[0x5];
	// 1 is little endian, 2 is big endian
	if(elf_data != 1 && elf_data != 2) {
		kernel_panic("Invalid ELF data class");
	}

	uint32_t elf_phoff = elf_endian(*reinterpret_cast<uint32_t*>(&buffer[0x1c]), elf_data);
	uint16_t elf_phentsize = elf_endian(*reinterpret_cast<uint16_t*>(&buffer[0x2a]), elf_data);
	elf_phnum = elf_endian(*reinterpret_cast<uint16_t*>(&buffer[0x2c]), elf_data);
	elf_ph_size = elf_phentsize * elf_phnum;
	if(elf_phoff + elf_ph_size > size) {
		kernel_panic("copy_and_map_elf has a buffer too small for all phdrs");
	}

	elf_phdr = reinterpret_cast<uint8_t*>(get_allocator()->allocate_aligned(elf_ph_size, 4096));
	memcpy(elf_phdr, &buffer[elf_phoff], elf_ph_size);
}

void process_fd::save_sse_state() {
	asm volatile("fxsave %0" : "=m" (sse_state));
}

void process_fd::restore_sse_state() {
	asm volatile("fxrstor %0" : "=m" (sse_state));
}