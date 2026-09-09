#!/usr/bin/env bash
# Run all three bounded product scenarios; each owns a unique evidence directory.
set -euo pipefail
if [ "$#" -ne 2 ]; then
  echo "usage: bash tests/v03_validate.sh PRODUCT EVIDENCE_ROOT" >&2
  exit 2
fi
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
for scene in tcp udp failure; do
  python3 "$script_dir/v03_system_test.py" "$1" "$2" "$scene"
done
