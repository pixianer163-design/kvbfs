#!/bin/bash
# KVBFS 卸载脚本

MOUNTPOINT="${1:-/tmp/kvbfs}"

fusermount -u "$MOUNTPOINT"
