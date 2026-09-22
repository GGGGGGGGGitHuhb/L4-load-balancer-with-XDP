#ifndef L4LB_UDP_DSR_SCHEMA_H_
#define L4LB_UDP_DSR_SCHEMA_H_
#include <linux/types.h>
#include <stddef.h>
#define L4LB_DSR_SCHEMA_VERSION 2U
#define L4LB_DSR_MAX_BACKENDS 64U

struct UdpDsrConfigValue {
  __u32 schemaVersion;
  __u32 backendCount;
  __u32 vipAddress;
  __u16 vipPort;
  __u16 reserved;
};

struct UdpDsrBackendValue {
  __u32 ifindex;
  __u8 destinationMac[6];
  __u8 sourceMac[6];
};

struct UdpDsrStatsValue {
  __u64 totalPackets;
  __u64 passPackets;
  __u64 redirectRequests;
  __u64 dropPackets;
  __u64 unsupportedPackets;
  __u64 noBackendPackets;
  __u64 invalidConfigPackets;
  __u64 helperErrorPackets;
};
#ifdef __cplusplus
#define L4LB_DSR_ASSERT static_assert
#define L4LB_DSR_ALIGN alignof
#else
#define L4LB_DSR_ASSERT _Static_assert
#define L4LB_DSR_ALIGN _Alignof
#endif
#define L4LB_DSR_OFFSET(type, field, offset) \
  L4LB_DSR_ASSERT(offsetof(struct type, field) == offset, #field)
L4LB_DSR_ASSERT(sizeof(struct UdpDsrConfigValue) == 16, "config size");
L4LB_DSR_ASSERT(L4LB_DSR_ALIGN(struct UdpDsrConfigValue) == 4, "config align");
L4LB_DSR_OFFSET(UdpDsrConfigValue, schemaVersion, 0);
L4LB_DSR_OFFSET(UdpDsrConfigValue, backendCount, 4);
L4LB_DSR_OFFSET(UdpDsrConfigValue, vipAddress, 8);
L4LB_DSR_OFFSET(UdpDsrConfigValue, vipPort, 12);
L4LB_DSR_OFFSET(UdpDsrConfigValue, reserved, 14);
L4LB_DSR_ASSERT(sizeof(struct UdpDsrBackendValue) == 16, "backend size");
L4LB_DSR_ASSERT(L4LB_DSR_ALIGN(struct UdpDsrBackendValue) == 4,
                "backend align");
L4LB_DSR_OFFSET(UdpDsrBackendValue, ifindex, 0);
L4LB_DSR_OFFSET(UdpDsrBackendValue, destinationMac, 4);
L4LB_DSR_OFFSET(UdpDsrBackendValue, sourceMac, 10);
L4LB_DSR_ASSERT(sizeof(struct UdpDsrStatsValue) == 64, "stats size");
L4LB_DSR_ASSERT(L4LB_DSR_ALIGN(struct UdpDsrStatsValue) == 8, "stats align");
L4LB_DSR_OFFSET(UdpDsrStatsValue, totalPackets, 0);
L4LB_DSR_OFFSET(UdpDsrStatsValue, passPackets, 8);
L4LB_DSR_OFFSET(UdpDsrStatsValue, redirectRequests, 16);
L4LB_DSR_OFFSET(UdpDsrStatsValue, dropPackets, 24);
L4LB_DSR_OFFSET(UdpDsrStatsValue, unsupportedPackets, 32);
L4LB_DSR_OFFSET(UdpDsrStatsValue, noBackendPackets, 40);
L4LB_DSR_OFFSET(UdpDsrStatsValue, invalidConfigPackets, 48);
L4LB_DSR_OFFSET(UdpDsrStatsValue, helperErrorPackets, 56);
#undef L4LB_DSR_OFFSET
#undef L4LB_DSR_ASSERT
#undef L4LB_DSR_ALIGN
#endif
