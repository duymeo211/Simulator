/**
 * @file    log_utils.c
 * @brief   File system helpers for creating and cleaning the log directory.
 * @date    2026-05
 */
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

void log_prepare_folder(const char* path)
{
#ifdef _WIN32
    /* create folder if not exist */
    CreateDirectoryA(path, NULL);

    /* delete all files inside */
    char pattern[256];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pattern, sizeof(pattern), "%s\\*.*", path);

    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s\\%s", path, fd.cFileName);
            DeleteFileA(filepath);
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);

#else
    mkdir(path, 0755);

    DIR* d = opendir(path);
    if (!d) return;

    struct dirent* dir;
    char filepath[256];

    while ((dir = readdir(d)) != NULL)
    {
        if (dir->d_type == DT_REG)
        {
            snprintf(filepath, sizeof(filepath), "%s/%s", path, dir->d_name);
            remove(filepath);
        }
    }
    closedir(d);
#endif
}