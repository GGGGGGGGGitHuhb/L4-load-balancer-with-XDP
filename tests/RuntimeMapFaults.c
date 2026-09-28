/* Test-only step failures; never linked into the product. */
#define _GNU_SOURCE
#include <bpf/bpf.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/bpf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int selected(const char* fault) {
  const char* value = getenv("L4LB_RUNTIME_FAULT");
  if (value && !strcmp(value, fault)) return 1;
  const char* path = getenv("L4LB_RUNTIME_FAULT_FILE");
  if (!path) return 0;
  FILE* file = fopen(path, "r");
  if (!file) return 0;
  char selectedFault[64] = {0};
  const int read = fscanf(file, "%63s", selectedFault);
  fclose(file);
  return read == 1 && !strcmp(selectedFault, fault);
}

static int named(int fd, const char* name) {
  struct bpf_map_info info = {0};
  uint32_t size = sizeof(info);
  int (*getInfo)(int, void*, uint32_t*) =
      dlsym(RTLD_NEXT, "bpf_obj_get_info_by_fd");
  return !getInfo(fd, &info, &size) && !strcmp(info.name, name);
}

int bpf_map_create(enum bpf_map_type type, const char* name, __u32 keySize,
                   __u32 valueSize, __u32 count,
                   const struct bpf_map_create_opts* options) {
  if (selected("create") && name && !strcmp(name, "l4lb_snap_v3")) {
    errno = ENOMEM;
    return -1;
  }
  int (*create)(enum bpf_map_type, const char*, __u32, __u32, __u32,
                const struct bpf_map_create_opts*) =
      dlsym(RTLD_NEXT, "bpf_map_create");
  return create(type, name, keySize, valueSize, count, options);
}

int bpf_map_update_elem(int fd, const void* key, const void* value,
                        __u64 flags) {
  if ((selected("write") && named(fd, "l4lb_snap_v3")) ||
      (selected("outer-update") && named(fd, "l4lb_active_v3"))) {
    errno = EIO;
    return -1;
  }
  int (*update)(int, const void*, const void*, __u64) =
      dlsym(RTLD_NEXT, "bpf_map_update_elem");
  return update(fd, key, value, flags);
}

int bpf_map_lookup_elem(int fd, const void* key, void* value) {
  if ((selected("read") && named(fd, "l4lb_snap_v3")) ||
      (selected("post-read") && named(fd, "l4lb_active_v3"))) {
    errno = EIO;
    return -1;
  }
  int (*lookup)(int, const void*, void*) =
      dlsym(RTLD_NEXT, "bpf_map_lookup_elem");
  int result = lookup(fd, key, value);
  if (!result && ((selected("mismatch") && named(fd, "l4lb_snap_v3")) ||
                  (selected("post-mismatch") && named(fd, "l4lb_active_v3"))))
    ((unsigned char*)value)[0] ^= 1;
  return result;
}

int bpf_map_freeze(int fd) {
  if (selected("freeze") && named(fd, "l4lb_snap_v3")) {
    errno = EIO;
    return -1;
  }
  int (*freeze)(int) = dlsym(RTLD_NEXT, "bpf_map_freeze");
  return freeze(fd);
}

int bpf_obj_get_info_by_fd(int fd, void* info, __u32* size) {
  if (selected("metadata") && named(fd, "l4lb_snap_v3")) {
    errno = EIO;
    return -1;
  }
  int (*getInfo)(int, void*, __u32*) =
      dlsym(RTLD_NEXT, "bpf_obj_get_info_by_fd");
  return getInfo(fd, info, size);
}
