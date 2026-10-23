// dirent.h for Windows builds: directory listing over FindFirstFileA (host/os_win.c).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

struct dirent { char d_name[260]; };
typedef struct RtDir DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

#ifdef __cplusplus
}
#endif
