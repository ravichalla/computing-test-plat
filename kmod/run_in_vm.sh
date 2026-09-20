#!/usr/bin/env bash
# Build, load, test and unload the valdev module.
#
# This loads an unsigned kernel module as root. Run it inside a throwaway VM
# (for example one booted by the vmtest harness), not on your workstation.
# Pass --force to run on bare metal anyway.
set -euo pipefail
cd "$(dirname "$0")"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

virt="$(systemd-detect-virt 2>/dev/null || true)"
if [[ "$virt" == "none" || -z "$virt" ]] && [[ $FORCE -eq 0 ]]; then
    echo "Refusing to load a test kernel module on what looks like bare metal." >&2
    echo "Use a VM, or re-run with --force if you know what you are doing." >&2
    exit 2
fi

make all

cleanup() { sudo rmmod valdev 2>/dev/null || true; }
trap cleanup EXIT

sudo rmmod valdev 2>/dev/null || true
sudo insmod ./valdev.ko
dmesg | tail -n 2 || true

status=0
./test_valdev || status=$?

echo "--- kernel log tail"
dmesg | tail -n 5 || true
exit "$status"
