#!/bin/bash

while true; do
    cat /proc/vmstat | grep numa_pages_migrated
    cat /proc/vmstat | grep pgmigrate_success
    cat /proc/vmstat | grep demote_anon
    cat /proc/vmstat | grep promote_anon
    sleep 1
done