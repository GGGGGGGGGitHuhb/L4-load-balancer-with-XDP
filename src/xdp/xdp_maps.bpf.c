#include <linux/bpf.h>

#include "MapSchema.h"

#define SEC(name) __attribute__((section(name), used))
#define L4LB_UINT(name, value) int(*name)[value]
#define L4LB_TYPE(name, value) value *name

struct {
  L4LB_UINT(type, BPF_MAP_TYPE_ARRAY);
  L4LB_UINT(max_entries, 1);
  L4LB_UINT(map_flags, BPF_F_RDONLY_PROG);
  L4LB_TYPE(key, __u32);
  L4LB_TYPE(value, struct XdpConfigValue);
} l4lb_cfg_v1 SEC(".maps");

struct {
  L4LB_UINT(type, BPF_MAP_TYPE_ARRAY);
  L4LB_UINT(max_entries, L4LB_XDP_MAX_BACKENDS);
  L4LB_UINT(map_flags, BPF_F_RDONLY_PROG);
  L4LB_TYPE(key, __u32);
  L4LB_TYPE(value, struct XdpBackendValue);
} l4lb_be_v1 SEC(".maps");

struct {
  L4LB_UINT(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  L4LB_UINT(max_entries, 1);
  L4LB_TYPE(key, __u32);
  L4LB_TYPE(value, struct XdpStatsValue);
} l4lb_stats_v1 SEC(".maps");

static void *(*const lookupElement)(void *, const void *) = (void *)
    BPF_FUNC_map_lookup_elem;

SEC("xdp")

int xdp_maps_pass(struct xdp_md *ctx) {
  (void)ctx;
  __u32 key = 0;
  // Retain configuration maps with the attached program, including after
  // SIGKILL. S1 deliberately does not inspect either value or choose a backend.
  lookupElement(&l4lb_cfg_v1, &key);
  lookupElement(&l4lb_be_v1, &key);
  struct XdpStatsValue *stats = lookupElement(&l4lb_stats_v1, &key);
  if (stats) __sync_fetch_and_add(&stats->passPackets, 1);
  return XDP_PASS;
}

char LICENSE[] SEC("license") = "GPL";
