#!/usr/bin/env bash
# Prepare a base guest image for the vmtest harness.
#
# The harness talks to the guest through the QEMU guest agent, so the image
# must have qemu-guest-agent installed. This script downloads an Ubuntu 24.04
# cloud image and bakes the agent in (cloud-init is disabled so the guest boots
# straight to a login prompt with no seed disk).
#
# Requirements on the host: curl, qemu-img (qemu-utils), virt-customize (libguestfs-tools).
# On Ubuntu hosts libguestfs needs a readable kernel:  sudo chmod 0644 /boot/vmlinuz-*
#
# Usage: scripts/prepare_image.sh [output.qcow2]
set -euo pipefail

OUT="${1:-vmtest-base.qcow2}"
URL="${IMAGE_URL:-https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img}"
SIZE="${IMAGE_SIZE:-6G}"

for tool in curl qemu-img virt-customize; do
    command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "==> downloading $URL"
curl -fL --progress-bar -o "$tmp/base.img" "$URL"

echo "==> converting to qcow2 and resizing to $SIZE"
qemu-img convert -O qcow2 "$tmp/base.img" "$OUT"
qemu-img resize -q "$OUT" "$SIZE"

echo "==> installing qemu-guest-agent"
virt-customize -a "$OUT" \
    --install qemu-guest-agent \
    --run-command 'systemctl enable qemu-guest-agent || true' \
    --run-command 'touch /etc/cloud/cloud-init.disabled'

echo "==> done: $OUT"
echo "    run:  build/vmtest --image $OUT --junit results.xml"
