if(BAREMETAL_ENABLED)
	add_library(fd
		fd.hpp
		memory_fd.cpp memory_fd.hpp
		mem_mapping.cpp mem_mapping.hpp
		process_fd.cpp process_fd.hpp
		scheduler.cpp scheduler.hpp scheduler.s
		procfs.cpp procfs.hpp
		bootfs.cpp bootfs.hpp
		initrdfs.cpp initrdfs.hpp
		thread.cpp thread.hpp
		pseudo_fd.cpp pseudo_fd.hpp
		sock.cpp sock.hpp
		unixsock.cpp unixsock.hpp
		userlandsock.cpp userlandsock.hpp
		ifstoresock.cpp ifstoresock.hpp
		rawsock.cpp rawsock.hpp
		reverse_fd.cpp reverse_fd.hpp
		shmfs.cpp shmfs.hpp
		blockdevstoresock.cpp blockdevstoresock.hpp
		vfs.cpp vfs.hpp
	)

	# for elf.h:
	target_include_directories(fd SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/cloudlibc/src/include")

	# for external_binaries.h:
	target_include_directories(fd PRIVATE "${CMAKE_BINARY_DIR}")
endif()
