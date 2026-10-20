#!/usr/bin/env bash
# 兼容入口：./tests/all_enhanced.sh
exec bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/enhanced/run_all.sh" "$@"
