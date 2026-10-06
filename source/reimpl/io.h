/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  io.h
 * @brief Wrappers and implementations for some of the IO functions.
 */

#ifndef SOLOADER_IO_H
#define SOLOADER_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>
#include <sys/dirent.h>
#include <sys/syslimits.h>
#include <sys/fcntl.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef DT_DIR
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14
#endif

typedef struct stat64_bionic {
    uint64_t st_dev;          // 0x00
    uint8_t __pad0[4];        // 0x08
    uint32_t __st_ino;        // 0x0c
    uint32_t st_mode;         // 0x10
    uint32_t st_nlink;        // 0x14
    uint32_t st_uid;          // 0x18
    uint32_t st_gid;          // 0x1c
    uint64_t st_rdev;         // 0x20
    uint8_t __pad3[8];        // 0x28
    int64_t st_size;          // 0x30
    uint32_t st_blksize;      // 0x38
    uint8_t __pad4[4];        // 0x3c
    uint64_t st_blocks;       // 0x40
    uint32_t st_atime_sec;    // 0x48
    uint32_t st_atime_nsec;   // 0x4c
    uint32_t st_mtime_sec;    // 0x50
    uint32_t st_mtime_nsec;   // 0x54
    uint32_t st_ctime_sec;    // 0x58
    uint32_t st_ctime_nsec;   // 0x5c
    uint64_t st_ino;          // 0x60
} stat64_bionic;

typedef struct __attribute__((__packed__)) dirent64_bionic {
    int16_t d_ino; // 2 bytes // offset 0x0
    int64_t d_off; // 8 bytes // offset 0x2
    uint64_t d_reclen; // 8 bytes // 0xA
    unsigned char d_type; // 1 byte // offset 0x12
    char d_name[256]; // 256 bytes // offset 0x13
} dirent64_bionic;

int open_soloader(const char * path, int oflag, ...);

int open64_soloader(const char * path, int oflag, ...);

FILE * fopen_soloader(const char * filename, const char * mode);

FILE * fopen64_soloader(const char * filename, const char * mode);

size_t fread_soloader(void *ptr, size_t size, size_t nmemb, FILE *stream);

size_t fwrite_soloader(const void *ptr, size_t size, size_t nmemb,
                       FILE *stream);

void rewind_soloader(FILE *stream);

int fseek_soloader(FILE *stream, long offset, int whence);

long ftell_soloader(FILE *stream);

int fseeko_soloader(FILE *stream, off_t offset, int whence);

off_t ftello_soloader(FILE *stream);

int fileno_soloader(FILE *stream);

DIR *opendir_soloader(char *name);

int stat_soloader(const char * path, stat64_bionic * buf);

int fstat_soloader(int fd, stat64_bionic * buf);

struct dirent64_bionic * readdir_soloader(DIR *dir);

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result);

int close_soloader(int fd);

ssize_t read_soloader(int fd, void *buf, size_t count);

off_t lseek_soloader(int fd, off_t offset, int whence);

off_t lseek64_soloader(int fd, off_t offset, int whence);

int fclose_soloader(FILE *f);

int closedir_soloader(DIR *dir);

int fcntl_soloader(int fd, int cmd, ...);

int ioctl_soloader(int fd, int request, ... /* arg */);

int fsync_soloader(int fd);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_IO_H
