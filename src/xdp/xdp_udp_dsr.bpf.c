#include <linux/bpf.h>

#include "UdpDsrSchema.h"
#define SEC(name) __attribute__((section(name), used))
#define L4LB_UINT(name, value) int(*name)[value]
#define L4LB_TYPE(name, value) value *name

struct {
  L4LB_UINT(type, BPF_MAP_TYPE_ARRAY);
  L4LB_UINT(max_entries, 1);
  L4LB_UINT(map_flags, BPF_F_RDONLY_PROG);
  L4LB_TYPE(key, __u32);
  L4LB_TYPE(value, struct UdpDsrConfigValue);
} l4lb_cfg_v2 SEC(".maps");

struct {
  L4LB_UINT(type, BPF_MAP_TYPE_ARRAY);
  L4LB_UINT(max_entries, 64);
  L4LB_UINT(map_flags, BPF_F_RDONLY_PROG);
  L4LB_TYPE(key, __u32);
  L4LB_TYPE(value, struct UdpDsrBackendValue);
} l4lb_be_v2 SEC(".maps");

struct {
  L4LB_UINT(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  L4LB_UINT(max_entries, 1);
  L4LB_TYPE(key, __u32);
  L4LB_TYPE(value, struct UdpDsrStatsValue);
} l4lb_stats_v2 SEC(".maps");

static void *(*const lookupElement)(void *, const void *) = (void *)
    BPF_FUNC_map_lookup_elem;
static long (*const redirectPacket)(__u32, __u64) = (void *)BPF_FUNC_redirect;

static __attribute__((always_inline)) inline int validMac(const __u8 *mac) {
  return !(mac[0] & 1) && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
}

SEC("xdp")

int xdp_udp_dsr(struct xdp_md *ctx) {
  __u32 key = 0;
  struct UdpDsrStatsValue *stats = lookupElement(&l4lb_stats_v2, &key);
  if (!stats) return XDP_PASS;
  __sync_fetch_and_add(&stats->totalPackets, 1);
  __u8 *data = (void *)(long)ctx->data;
  __u8 *end = (void *)(long)ctx->data_end;
  // Fixed headers first; every following byte access is covered by this bound.
  if (data + 42 > end) goto unsupported;
  if (data[12] != 8 || data[13] != 0 || data[14] != 0x45 || data[23] != 17 ||
      (data[20] & 0xbf) || data[21])
    goto unsupported;
  __u32 ipLength = ((__u32)data[16] << 8) | data[17];
  __u32 udpLength = ((__u32)data[38] << 8) | data[39];
  if (ipLength < 28 || ipLength > 1500 || data + 14 + ipLength > end ||
      udpLength != ipLength - 20)
    goto unsupported;
  __u32 checksum = 0;
#pragma unroll
  for (int i = 14; i < 34; i += 2)
    checksum += ((__u32)data[i] << 8) | data[i + 1];
  checksum = (checksum & 0xffff) + (checksum >> 16);
  checksum = (checksum & 0xffff) + (checksum >> 16);
  if (checksum != 0xffff) goto unsupported;
  struct UdpDsrConfigValue *config = lookupElement(&l4lb_cfg_v2, &key);
  if (!config || config->schemaVersion != 2 || config->backendCount > 64 ||
      config->reserved)
    goto invalidConfig;
  __u32 address;
  __u16 port;
  __builtin_memcpy(&address, data + 30, 4);
  __builtin_memcpy(&port, data + 36, 2);
  if (address != config->vipAddress || port != config->vipPort)
    goto unsupported;
  __u32 count = config->backendCount;
  if (!count) {
    __sync_fetch_and_add(&stats->noBackendPackets, 1);
    goto pass;
  }
  __u32 hash = 2166136261U;
#pragma unroll
  for (int i = 26; i < 38; ++i) hash = (hash ^ data[i]) * 16777619U;
  hash = (hash ^ 17U) * 16777619U;
  key = hash % count;
  struct UdpDsrBackendValue *backend = lookupElement(&l4lb_be_v2, &key);
  if (!backend || !backend->ifindex || !validMac(backend->destinationMac) ||
      !validMac(backend->sourceMac))
    goto invalidConfig;
  if (redirectPacket(backend->ifindex, 0) != XDP_REDIRECT) {
    __sync_fetch_and_add(&stats->helperErrorPackets, 1);
    __sync_fetch_and_add(&stats->dropPackets, 1);
    return XDP_DROP;
  }
  __builtin_memcpy(data, backend->destinationMac, 6);
  __builtin_memcpy(data + 6, backend->sourceMac, 6);
  __sync_fetch_and_add(&stats->redirectRequests, 1);
  return XDP_REDIRECT;
invalidConfig:
  __sync_fetch_and_add(&stats->invalidConfigPackets, 1);
  goto pass;
unsupported:
  __sync_fetch_and_add(&stats->unsupportedPackets, 1);
pass:
  __sync_fetch_and_add(&stats->passPackets, 1);
  return XDP_PASS;
}

char LICENSE[] SEC("license") = "GPL";
