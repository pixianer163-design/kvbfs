#!/bin/bash
# KVBFS 挂载脚本

MOUNTPOINT="${1:-/tmp/kvbfs}"
KVSTORE="${2:-/tmp/kvbfs_data}"

mkdir -p "$MOUNTPOINT"
mkdir -p "$KVSTORE"

./kvbfs "$MOUNTPOINT" "$KVSTORE" -f
