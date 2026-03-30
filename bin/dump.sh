#!/bin/bash
ID="${1:-qs}"
{ coredumpctl info "$ID"; coredumpctl debug "$ID" -A "-batch -ex 'thread apply all bt full' -ex q" 2>&1; } | tee ~/qs-crash-$(date +%s).txt
