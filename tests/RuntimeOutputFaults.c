/* Test-only stdout failures. Never linked into the production loader. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int selected(const char* mode) {
  const char* path = getenv("L4LB_OUTPUT_FAULT_FILE");
  if (!path) return 0;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  char value[64] = {0};
  ssize_t count = read(fd, value, sizeof(value) - 1);
  close(fd);
  return count > 0 && !strcmp(value, mode);
}

static void record(const char* text) {
  const char* path = getenv("L4LB_OUTPUT_FAULT_TRACE");
  if (!path) return;
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  syscall(SYS_write, fd, text, strlen(text));
  close(fd);
}

ssize_t write(int fd, const void* buffer, size_t size) {
  static size_t largestPending = 0;
  static int failedUnchanged = 0;
  ssize_t (*realWrite)(int, const void*, size_t) = dlsym(RTLD_NEXT, "write");
  if (fd == STDOUT_FILENO && selected("eagain")) {
    if (size > largestPending) {
      char text[64];
      largestPending = size;
      snprintf(text, sizeof(text), "%zu\n", largestPending);
      record(text);
    }
    errno = EAGAIN;
    return -1;
  }
  static const char kUnchanged[] = "XDP_RELOAD status=unchanged";
  if (fd == STDOUT_FILENO && !failedUnchanged && selected("unchanged-eio") &&
      memmem(buffer, size, kUnchanged, sizeof(kUnchanged) - 1)) {
    failedUnchanged = 1;
    record("unchanged_eio_once\n");
    errno = EIO;
    return -1;
  }
  return realWrite(fd, buffer, size);
}
