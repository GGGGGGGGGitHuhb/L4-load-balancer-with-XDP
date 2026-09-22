/* Test-only interposition. Never linked into or enabled by the product. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/bpf.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int faultMatches(int fd, const char *fault, const char *mapName) {
  const char *selected = getenv("L4LB_TEST_MAP_FAULT");
  if (!selected || strcmp(selected, fault)) return 0;
  struct bpf_map_info info = {0};
  uint32_t size = sizeof(info);
  int (*getInfo)(int, void *, uint32_t *) =
      dlsym(RTLD_NEXT, "bpf_obj_get_info_by_fd");
  return getInfo && !getInfo(fd, &info, &size) && !strcmp(info.name, mapName);
}

int bpf_map_update_elem(int fd, const void *key, const void *value,
                        uint64_t flags) {
  if ((faultMatches(fd, "backend-write", "l4lb_be_v1") &&
       *(const uint32_t *)key == 1) ||
      faultMatches(fd, "config-write", "l4lb_cfg_v1")) {
    errno = EIO;
    return -1;
  }
  int (*update)(int, const void *, const void *, uint64_t) =
      dlsym(RTLD_NEXT, "bpf_map_update_elem");
  return update(fd, key, value, flags);
}

int bpf_map_lookup_elem(int fd, const void *key, void *value) {
  if (faultMatches(fd, "stats-read", "l4lb_stats_v1") ||
      faultMatches(fd, "config-read-error", "l4lb_cfg_v1")) {
    errno = EIO;
    return -1;
  }
  int (*lookup)(int, const void *, void *) =
      dlsym(RTLD_NEXT, "bpf_map_lookup_elem");
  int result = lookup(fd, key, value);
  if (!result && (faultMatches(fd, "config-mismatch", "l4lb_cfg_v1") ||
                  (faultMatches(fd, "backend-mismatch", "l4lb_be_v1") &&
                   *(const uint32_t *)key == 3))) {
    ((unsigned char *)value)[0] ^= 1;
  }
  return result;
}

int bpf_map_freeze(int fd) {
  if (faultMatches(fd, "backend-freeze", "l4lb_be_v1") ||
      faultMatches(fd, "config-freeze", "l4lb_cfg_v1")) {
    errno = EIO;
    return -1;
  }
  int (*freeze)(int) = dlsym(RTLD_NEXT, "bpf_map_freeze");
  return freeze(fd);
}
