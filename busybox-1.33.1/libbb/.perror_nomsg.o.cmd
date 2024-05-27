cmd_libbb/perror_nomsg.o := riscv64-linux-gnu-gcc -Wp,-MD,libbb/.perror_nomsg.o.d  -std=gnu99 -Iinclude -Ilibbb  -include include/autoconf.h -D_GNU_SOURCE -DNDEBUG -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -DBB_VER='"1.33.1"' -Wall -Wshadow -Wwrite-strings -Wundef -Wstrict-prototypes -Wunused -Wunused-parameter -Wunused-function -Wunused-value -Wmissing-prototypes -Wmissing-declarations -Wno-format-security -Wdeclaration-after-statement -Wold-style-definition -finline-limit=0 -fno-builtin-strlen -fomit-frame-pointer -ffunction-sections -fdata-sections -fno-guess-branch-probability -funsigned-char -static-libgcc -falign-functions=1 -falign-jumps=1 -falign-labels=1 -falign-loops=1 -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-builtin-printf -Os    -DKBUILD_BASENAME='"perror_nomsg"'  -DKBUILD_MODNAME='"perror_nomsg"' -c -o libbb/perror_nomsg.o libbb/perror_nomsg.c

deps_libbb/perror_nomsg.o := \
  libbb/perror_nomsg.c \
  /usr/riscv64-linux-gnu/include/stdc-predef.h \
  include/platform.h \
    $(wildcard include/config/werror.h) \
    $(wildcard include/config/big/endian.h) \
    $(wildcard include/config/little/endian.h) \
    $(wildcard include/config/nommu.h) \
  /usr/lib/gcc-cross/riscv64-linux-gnu/11/include/limits.h \
  /usr/lib/gcc-cross/riscv64-linux-gnu/11/include/syslimits.h \
  /usr/riscv64-linux-gnu/include/limits.h \
  /usr/riscv64-linux-gnu/include/bits/libc-header-start.h \
  /usr/riscv64-linux-gnu/include/features.h \
  /usr/riscv64-linux-gnu/include/features-time64.h \
  /usr/riscv64-linux-gnu/include/bits/wordsize.h \
  /usr/riscv64-linux-gnu/include/bits/timesize.h \
  /usr/riscv64-linux-gnu/include/sys/cdefs.h \
  /usr/riscv64-linux-gnu/include/bits/long-double.h \
  /usr/riscv64-linux-gnu/include/gnu/stubs.h \
  /usr/riscv64-linux-gnu/include/gnu/stubs-lp64d.h \
  /usr/riscv64-linux-gnu/include/bits/posix1_lim.h \
  /usr/riscv64-linux-gnu/include/bits/local_lim.h \
  /usr/riscv64-linux-gnu/include/linux/limits.h \
  /usr/riscv64-linux-gnu/include/bits/pthread_stack_min-dynamic.h \
  /usr/riscv64-linux-gnu/include/bits/posix2_lim.h \
  /usr/riscv64-linux-gnu/include/bits/xopen_lim.h \
  /usr/riscv64-linux-gnu/include/bits/uio_lim.h \
  /usr/riscv64-linux-gnu/include/byteswap.h \
  /usr/riscv64-linux-gnu/include/bits/byteswap.h \
  /usr/riscv64-linux-gnu/include/bits/types.h \
  /usr/riscv64-linux-gnu/include/bits/typesizes.h \
  /usr/riscv64-linux-gnu/include/bits/time64.h \
  /usr/riscv64-linux-gnu/include/endian.h \
  /usr/riscv64-linux-gnu/include/bits/endian.h \
  /usr/riscv64-linux-gnu/include/bits/endianness.h \
  /usr/riscv64-linux-gnu/include/bits/uintn-identity.h \
  /usr/lib/gcc-cross/riscv64-linux-gnu/11/include/stdint.h \
  /usr/riscv64-linux-gnu/include/stdint.h \
  /usr/riscv64-linux-gnu/include/bits/wchar.h \
  /usr/riscv64-linux-gnu/include/bits/stdint-intn.h \
  /usr/riscv64-linux-gnu/include/bits/stdint-uintn.h \
  /usr/lib/gcc-cross/riscv64-linux-gnu/11/include/stdbool.h \
  /usr/riscv64-linux-gnu/include/unistd.h \
  /usr/riscv64-linux-gnu/include/bits/posix_opt.h \
  /usr/riscv64-linux-gnu/include/bits/environments.h \
  /usr/lib/gcc-cross/riscv64-linux-gnu/11/include/stddef.h \
  /usr/riscv64-linux-gnu/include/bits/confname.h \
  /usr/riscv64-linux-gnu/include/bits/getopt_posix.h \
  /usr/riscv64-linux-gnu/include/bits/getopt_core.h \
  /usr/riscv64-linux-gnu/include/bits/unistd.h \
  /usr/riscv64-linux-gnu/include/bits/unistd_ext.h \
  /usr/riscv64-linux-gnu/include/linux/close_range.h \

libbb/perror_nomsg.o: $(deps_libbb/perror_nomsg.o)

$(deps_libbb/perror_nomsg.o):
