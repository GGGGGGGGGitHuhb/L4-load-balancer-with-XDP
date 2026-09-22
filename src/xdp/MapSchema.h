#ifndef L4LB_XDP_MAP_SCHEMA_H_
#define L4LB_XDP_MAP_SCHEMA_H_
#include <linux/types.h>
#include <stddef.h>

#define L4LB_XDP_SCHEMA_VERSION 1U
#define L4LB_XDP_MAX_BACKENDS 64U

struct XdpConfigValue {
  __u32 schemaVersion;
  __u32 backendCount;
};

struct XdpBackendValue {
  __u32 address;
  __u16 port;
  __u16 reserved;
};

struct XdpStatsValue {
  __u64 passPackets;
};
#ifdef __cplusplus
#define L4LB_ABI_ASSERT static_assert
#define L4LB_ABI_ALIGN alignof
#else
#define L4LB_ABI_ASSERT _Static_assert
#define L4LB_ABI_ALIGN _Alignof
#endif
L4LB_ABI_ASSERT(sizeof(struct XdpConfigValue) == 8, "config size");
L4LB_ABI_ASSERT(L4LB_ABI_ALIGN(struct XdpConfigValue) == 4, "config alignment");
L4LB_ABI_ASSERT(offsetof(struct XdpConfigValue, schemaVersion) == 0,
                "schema offset");
L4LB_ABI_ASSERT(offsetof(struct XdpConfigValue, backendCount) == 4,
                "count offset");
L4LB_ABI_ASSERT(sizeof(struct XdpBackendValue) == 8, "backend size");
L4LB_ABI_ASSERT(L4LB_ABI_ALIGN(struct XdpBackendValue) == 4,
                "backend alignment");
L4LB_ABI_ASSERT(offsetof(struct XdpBackendValue, address) == 0,
                "address offset");
L4LB_ABI_ASSERT(offsetof(struct XdpBackendValue, port) == 4, "port offset");
L4LB_ABI_ASSERT(offsetof(struct XdpBackendValue, reserved) == 6,
                "reserved offset");
L4LB_ABI_ASSERT(sizeof(struct XdpStatsValue) == 8, "stats size");
L4LB_ABI_ASSERT(L4LB_ABI_ALIGN(struct XdpStatsValue) == 8, "stats alignment");
L4LB_ABI_ASSERT(offsetof(struct XdpStatsValue, passPackets) == 0,
                "stats offset");
#undef L4LB_ABI_ASSERT
#undef L4LB_ABI_ALIGN
#endif
