// unistd.h for Windows builds: the POSIX calls the host code uses, mapped to the C runtime
// (UCRT, MinGW and MSVC alike) or to host/os_win.c. Also the few names from <time.h>,
// <string.h>, <stdio.h> and <sys/stat.h> that POSIX has and the Windows C runtime spells
// differently, since every file that uses them includes this one.
#pragma once
#include <direct.h>
#include <errno.h>
#include <io.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

int usleep(unsigned us);                            // high-resolution timer (os_win.c)
typedef unsigned useconds_t;

#ifndef F_OK
#define F_OK 0
#endif
#define access _access
#define getpid _getpid
#define fileno _fileno
#define ftruncate(fd, n) _chsize_s((fd), (n))
#define fseeko _fseeki64
#define ftello _ftelli64
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define mkdir(path, mode) _mkdir(path)
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

static inline struct tm *localtime_r(const time_t *t, struct tm *tm) { return localtime_s(tm, t) ? NULL : tm; }

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
typedef int clockid_t;
#endif
int rt_clock_gettime(int clock, struct timespec *ts);
#define clock_gettime rt_clock_gettime

#ifdef __cplusplus
}
#endif
