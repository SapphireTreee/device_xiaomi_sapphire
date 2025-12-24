#!/vendor/bin/sh
#
# Copyright (C) 2024 Paranoid Android
# SPDX-License-Identifier: Apache-2.0
#

function write_irq_affinity() {
    # Arguments:
    # $1 = irq name
    # $2 = cpu id
    irq_dir="$(dirname /proc/irq/*/$1)"
    [ -d "$irq_dir" ] && echo $2 > "${irq_dir}/smp_affinity_list"
}

# 1. IRQ Tuning (Keeps UI threads from being interrupted)
write_irq_affinity kgsl_3d0_irq 1
write_irq_affinity msm_drm 2
write_irq_affinity kgsl-3d0 1
write_irq_affinity MDSS 2

# 2. I/O Optimizations (Fixes lag during Notification/Uninstallation)
# This reduces "I/O wait" which causes the UI to stutter when the disk is busy.
for queue in /sys/block/*/queue/
do
    # Disable entropy contribution for less CPU overhead
    echo 0 > "${queue}add_random"
    
    # Disable request merging for lower latency (instant execution)
    echo 1 > "${queue}nomerges"
    
    # Increase the number of allowable requests to prevent bottlenecks
    echo 512 > "${queue}nr_requests"
    
    # Prioritize low latency if the scheduler supports it
    if [ -f "${queue}iosched/low_latency" ]; then
        echo 1 > "${queue}iosched/low_latency"
    fi
done

# 3. WALT Governor Aggression
# Since you switched to WALT, this helps the governor "see" touch events faster.
echo 1 > /proc/sys/walt/sched_boost