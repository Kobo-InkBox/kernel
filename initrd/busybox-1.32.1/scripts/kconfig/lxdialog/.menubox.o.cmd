cmd_scripts/kconfig/lxdialog/menubox.o := gcc -Wp,-MD,scripts/kconfig/lxdialog/.menubox.o.d  -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer   -I/usr/include/ncursesw -DCURSES_LOC="<curses.h>"  -DNCURSES_WIDECHAR=1 -DLOCALE    -c -o scripts/kconfig/lxdialog/menubox.o scripts/kconfig/lxdialog/menubox.c

deps_scripts/kconfig/lxdialog/menubox.o := \
  scripts/kconfig/lxdialog/menubox.c \
  /usr/include/stdc-predef.h \
  scripts/kconfig/lxdialog/dialog.h \
  /usr/include/i386-linux-gnu/sys/types.h \
  /usr/include/features.h \
  /usr/include/i386-linux-gnu/sys/cdefs.h \
  /usr/include/i386-linux-gnu/bits/wordsize.h \
  /usr/include/i386-linux-gnu/bits/long-double.h \
  /usr/include/i386-linux-gnu/gnu/stubs.h \
  /usr/include/i386-linux-gnu/gnu/stubs-32.h \
  /usr/include/i386-linux-gnu/bits/types.h \
  /usr/include/i386-linux-gnu/bits/typesizes.h \
  /usr/include/i386-linux-gnu/bits/types/clock_t.h \
  /usr/include/i386-linux-gnu/bits/types/clockid_t.h \
  /usr/include/i386-linux-gnu/bits/types/time_t.h \
  /usr/include/i386-linux-gnu/bits/types/timer_t.h \
  /usr/lib/gcc/i686-linux-gnu/8/include/stddef.h \
  /usr/include/i386-linux-gnu/bits/stdint-intn.h \
  /usr/include/endian.h \
  /usr/include/i386-linux-gnu/bits/endian.h \
  /usr/include/i386-linux-gnu/bits/byteswap.h \
  /usr/include/i386-linux-gnu/bits/uintn-identity.h \
  /usr/include/i386-linux-gnu/sys/select.h \
  /usr/include/i386-linux-gnu/bits/select.h \
  /usr/include/i386-linux-gnu/bits/types/sigset_t.h \
  /usr/include/i386-linux-gnu/bits/types/__sigset_t.h \
  /usr/include/i386-linux-gnu/bits/types/struct_timeval.h \
  /usr/include/i386-linux-gnu/bits/types/struct_timespec.h \
  /usr/include/i386-linux-gnu/bits/pthreadtypes.h \
  /usr/include/i386-linux-gnu/bits/thread-shared-types.h \
  /usr/include/i386-linux-gnu/bits/pthreadtypes-arch.h \
  /usr/include/fcntl.h \
  /usr/include/i386-linux-gnu/bits/fcntl.h \
  /usr/include/i386-linux-gnu/bits/fcntl-linux.h \
  /usr/include/i386-linux-gnu/bits/stat.h \
  /usr/include/unistd.h \
  /usr/include/i386-linux-gnu/bits/posix_opt.h \
  /usr/include/i386-linux-gnu/bits/environments.h \
  /usr/include/i386-linux-gnu/bits/confname.h \
  /usr/include/i386-linux-gnu/bits/getopt_posix.h \
  /usr/include/i386-linux-gnu/bits/getopt_core.h \
  /usr/include/ctype.h \
  /usr/include/i386-linux-gnu/bits/types/locale_t.h \
  /usr/include/i386-linux-gnu/bits/types/__locale_t.h \
  /usr/include/stdlib.h \
  /usr/include/i386-linux-gnu/bits/libc-header-start.h \
  /usr/include/i386-linux-gnu/bits/waitflags.h \
  /usr/include/i386-linux-gnu/bits/waitstatus.h \
  /usr/include/i386-linux-gnu/bits/floatn.h \
  /usr/include/i386-linux-gnu/bits/floatn-common.h \
  /usr/include/alloca.h \
  /usr/include/i386-linux-gnu/bits/stdlib-bsearch.h \
  /usr/include/i386-linux-gnu/bits/stdlib-float.h \
  /usr/include/string.h \
  /usr/include/strings.h \
  /usr/include/ncursesw/curses.h \
  /usr/include/ncursesw/ncurses_dll.h \
  /usr/lib/gcc/i686-linux-gnu/8/include/stdint.h \
  /usr/include/stdint.h \
  /usr/include/i386-linux-gnu/bits/wchar.h \
  /usr/include/i386-linux-gnu/bits/stdint-uintn.h \
  /usr/include/stdio.h \
  /usr/lib/gcc/i686-linux-gnu/8/include/stdarg.h \
  /usr/include/i386-linux-gnu/bits/types/__fpos_t.h \
  /usr/include/i386-linux-gnu/bits/types/__mbstate_t.h \
  /usr/include/i386-linux-gnu/bits/types/__fpos64_t.h \
  /usr/include/i386-linux-gnu/bits/types/__FILE.h \
  /usr/include/i386-linux-gnu/bits/types/FILE.h \
  /usr/include/i386-linux-gnu/bits/types/struct_FILE.h \
  /usr/include/i386-linux-gnu/bits/stdio_lim.h \
  /usr/include/i386-linux-gnu/bits/sys_errlist.h \
  /usr/include/i386-linux-gnu/bits/stdio.h \
  /usr/lib/gcc/i686-linux-gnu/8/include/stdbool.h \
  /usr/include/wchar.h \
  /usr/include/i386-linux-gnu/bits/types/wint_t.h \
  /usr/include/i386-linux-gnu/bits/types/mbstate_t.h \
  /usr/include/ncursesw/unctrl.h \

scripts/kconfig/lxdialog/menubox.o: $(deps_scripts/kconfig/lxdialog/menubox.o)

$(deps_scripts/kconfig/lxdialog/menubox.o):
