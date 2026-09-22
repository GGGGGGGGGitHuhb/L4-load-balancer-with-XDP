#!/usr/bin/env python3
"""Unprivileged maps CLI/ELF rejection; never asks the kernel to attach."""
import argparse
import pathlib
import subprocess
import tempfile


def run_rejected(loader, arguments, expected):
    result = subprocess.run([loader, *arguments], capture_output=True, text=True, timeout=10)
    output = result.stdout + result.stderr
    assert result.returncode != 0 and "READY" not in output, output
    assert any(fragment in output for fragment in ([expected] if isinstance(expected, str) else expected)), output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--loader", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--source", required=True)
    args = parser.parse_args()
    source = pathlib.Path(args.source)
    base = ["attach", "--dev", "lo", "--object", "missing", "--maps"]
    for options in (["--maps"], ["--backend"], ["--backend", "0.0.0.0:9"],
                    ["--backend", "1.2.3.4:8", "--backend", "1.2.3.4:08"],
                    sum((["--backend", f"10.0.0.1:{index+1}"] for index in range(65)), [])):
        run_rejected(args.loader, base + options, "参数错误")
    run_rejected(args.loader, base[:-1] + ["--backend", "1.2.3.4:8"], "参数错误")
    run_rejected(args.loader, ["detach", "--dev", "lo", "--prog-id", "1", "--maps"], "参数错误")
    run_rejected(args.loader, ["detach", "--dev", "lo", "--prog-id", "1", "--backend", "1.2.3.4:8"], "参数错误")
    original = source.read_text()
    mutations = {
        "name": original.replace("l4lb_be_v1", "l4lb_be_v2"),
        "type": original.replace("L4LB_UINT(type, BPF_MAP_TYPE_ARRAY)", "L4LB_UINT(type, BPF_MAP_TYPE_HASH)", 1),
        "key": original.replace("L4LB_TYPE(key, __u32)", "L4LB_TYPE(key, __u64)", 1),
        "value": original.replace("L4LB_TYPE(value, struct XdpConfigValue)", "L4LB_TYPE(value, __u32)", 1),
        "capacity": original.replace("L4LB_UINT(max_entries, L4LB_XDP_MAX_BACKENDS)", "L4LB_UINT(max_entries, 63)"),
        "flags": original.replace("L4LB_UINT(map_flags, BPF_F_RDONLY_PROG)", "L4LB_UINT(map_flags, 0)", 1),
        "missing": original.replace("l4lb_cfg_v1 SEC(\".maps\")", "unused_cfg").replace("lookupElement(&l4lb_cfg_v1, &key);", ""),
        "extra": original + '\nstruct { L4LB_UINT(type, BPF_MAP_TYPE_ARRAY); L4LB_UINT(max_entries, 1); L4LB_TYPE(key, __u32); L4LB_TYPE(value, __u32); } extra SEC(".maps");\n',
        "global": original.replace("(void)ctx;", "(void)ctx; extraGlobal++;").replace('SEC("xdp")', 'volatile int extraGlobal;\nSEC("xdp")'),
        "extra_program": original + '\nSEC("xdp") int other_program(struct xdp_md *ctx) { (void)ctx; return XDP_PASS; }\n',
        "program": original.replace("xdp_maps_pass", "wrong_program"),
        "section": original.replace('SEC("xdp")', 'SEC("socket")'),
    }
    with tempfile.TemporaryDirectory(prefix="xdp-maps-cli-") as directory:
        for name, contents in mutations.items():
            fixture = pathlib.Path(directory) / f"{name}.c"
            obj = fixture.with_suffix(".o")
            fixture.write_text(contents)
            subprocess.run([args.clang, "-target", "bpf", "-O2", "-g", "-I", str(source.parent),
                            "-isystem", "/usr/include/" + subprocess.check_output(["gcc", "-dumpmachine"], text=True).strip(),
                            "-c", str(fixture), "-o", str(obj)], check=True, capture_output=True)
            run_rejected(args.loader, ["attach", "--dev", "lo", "--object", str(obj), "--maps"],
                         "对象应仅包含" if name in ("program", "section", "extra_program") else
                         ("map 白名单不匹配", "maps 模式必须", "缺少 map"))
    print("maps CLI and 12 object whitelist mutations passed")


if __name__ == "__main__":
    main()
