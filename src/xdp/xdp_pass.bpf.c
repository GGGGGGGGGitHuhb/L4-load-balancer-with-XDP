#include <linux/bpf.h>

// Keep this build-only skeleton independent of libbpf development headers.
#define SEC(name) __attribute__((section(name), used))

/** Minimal XDP entry: allow every packet to continue through the network stack.
 */
SEC("xdp")

int xdp_pass(struct xdp_md *ctx) {
  (void)ctx;
  return XDP_PASS;
}

// The symbol must differ from the ELF section name for Clang's BPF assembler.
char LICENSE[] SEC("license") = "GPL";
