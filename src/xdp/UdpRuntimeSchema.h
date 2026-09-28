#ifndef L4LB_UDP_RUNTIME_SCHEMA_H_
#define L4LB_UDP_RUNTIME_SCHEMA_H_
#include "UdpDsrSchema.h"
#define L4LB_RUNTIME_SCHEMA_VERSION 3U

/** One immutable configuration generation, published through one outer slot. */
struct UdpRuntimeSnapshot {
  __u32 schemaVersion;
  __u32 backendCount;
  __u64 generation;
  __u32 vipAddress;
  __u16 vipPort;
  __u16 reserved;
  struct UdpDsrBackendValue backends[L4LB_DSR_MAX_BACKENDS];
};
#ifdef __cplusplus
#define L4LB_RUNTIME_ASSERT static_assert
#define L4LB_RUNTIME_ALIGN alignof
#else
#define L4LB_RUNTIME_ASSERT _Static_assert
#define L4LB_RUNTIME_ALIGN _Alignof
#endif
#define L4LB_RUNTIME_OFFSET(field, offset)                                  \
  L4LB_RUNTIME_ASSERT(offsetof(struct UdpRuntimeSnapshot, field) == offset, \
                      #field)
L4LB_RUNTIME_ASSERT(sizeof(struct UdpRuntimeSnapshot) == 1048, "snapshot size");
L4LB_RUNTIME_ASSERT(L4LB_RUNTIME_ALIGN(struct UdpRuntimeSnapshot) == 8,
                    "snapshot align");
L4LB_RUNTIME_OFFSET(schemaVersion, 0);
L4LB_RUNTIME_OFFSET(backendCount, 4);
L4LB_RUNTIME_OFFSET(generation, 8);
L4LB_RUNTIME_OFFSET(vipAddress, 16);
L4LB_RUNTIME_OFFSET(vipPort, 20);
L4LB_RUNTIME_OFFSET(reserved, 22);
L4LB_RUNTIME_OFFSET(backends, 24);
#undef L4LB_RUNTIME_OFFSET
#undef L4LB_RUNTIME_ASSERT
#undef L4LB_RUNTIME_ALIGN
#endif
