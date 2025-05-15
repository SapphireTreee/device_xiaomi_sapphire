#!/system/bin/sh
# Copyright (c) 2020-2022 Qualcomm Technologies, Inc.
# All Rights Reserved.
# Confidential and Proprietary - Qualcomm Technologies, Inc.
#
# Copyright (c) 2009-2012, 2014-2019, The Linux Foundation. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of The Linux Foundation nor
#       the names of its contributors may be used to endorse or promote
#       products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
# OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# Initialize kernel version
KernelVersionStr=`cat /proc/sys/kernel/osrelease`
KernelVersionA=${KernelVersionStr:0:1}
KernelVersionS=${KernelVersionStr:2:2}
KernelVersionB=${KernelVersionS%.*}

function configure_zram_parameters() {
    MemTotalStr=`cat /proc/meminfo | grep MemTotal`
    MemTotal=${MemTotalStr:16:8}
    low_ram=`getprop ro.config.low_ram`

    # Zram disk: 75% for Go devices (512MB, 1GB, 2GB).
    # For >2GB non-Go devices, size = 33% of RAM, capped at 2560MB.
    # Set max_comp_streams to 4 for Snapdragon 685 efficiency.
    # Disable deduplication to reduce CPU overhead.
    # Use lz4 compression for Go devices.

    let RamSizeGB="( $MemTotal / 1048576 ) + 1"
    diskSizeUnit=M
    if [ $RamSizeGB -le 2 ]; then
        let zRamSizeMB="( $RamSizeGB * 1024 ) * 3 / 4"
    else
        let zRamSizeMB="( $RamSizeGB * 1024 ) / 3"
    fi

    # Cap zRAM size at 2560 MB
    if [ $zRamSizeMB -gt 2560 ]; then
        let zRamSizeMB=2560
    fi

    if [ "$low_ram" == "true" ]; then
        echo lz4 > /sys/block/zram0/comp_algorithm || echo "zRAM: Failed to set lz4 algorithm" > /dev/kmsg
    fi

    if [ -f /sys/block/zram0/disksize ]; then
        if [ -f /sys/block/zram0/use_dedup ]; then
            echo 0 > /sys/block/zram0/use_dedup || echo "zRAM: Failed to disable deduplication" > /dev/kmsg
        fi
        echo 4 > /sys/block/zram0/max_comp_streams || echo "zRAM: Failed to set max_comp_streams to 4" > /dev/kmsg
        echo "$zRamSizeMB""$diskSizeUnit" > /sys/block/zram0/disksize || echo "zRAM: Failed to set disksize to $zRamSizeMB$diskSizeUnit" > /dev/kmsg

        # Disable SLAB_STORE_USER debug to reduce memory overhead
        if [ -e /sys/kernel/slab/zs_handle ]; then
            echo 0 > /sys/kernel/slab/zs_handle/store_user
        fi
        if [ -e /sys/kernel/slab/zspage ]; then
            echo 0 > /sys/kernel/slab/zspage/store_user
        fi

        mkswap /dev/block/zram0 || echo "zRAM: Failed to create swap" > /dev/kmsg
        swapon /dev/block/zram0 -p 32758 || echo "zRAM: Failed to enable swap" > /dev/kmsg
        echo "zRAM: size=${zRamSizeMB}MB, streams=4, dedup=0" > /dev/kmsg
    else
        echo "zRAM: /sys/block/zram0/disksize not found" > /dev/kmsg
    fi
}

function configure_read_ahead_kb_values() {
    MemTotalStr=`cat /proc/meminfo | grep MemTotal`
    MemTotal=${MemTotalStr:16:8}
    dmpts=$(ls /sys/block/*/queue/read_ahead_kb | grep -e dm -e mmc -e sd)

    # Set read-ahead: 128 KB for ≤3GB, 512 KB for ≥4GB
    if [ $MemTotal -le 3145728 ]; then
        ra_kb=128
    else
        ra_kb=512
    fi
    if [ -f /sys/block/mmcblk0/bdi/read_ahead_kb ]; then
        echo $ra_kb > /sys/block/mmcblk0/bdi/read_ahead_kb || echo "read_ahead: Failed for mmcblk0" > /dev/kmsg
    fi
    if [ -f /sys/block/mmcblk0rpmb/bdi/read_ahead_kb ]; then
        echo $ra_kb > /sys/block/mmcblk0rpmb/bdi/read_ahead_kb || echo "read_ahead: Failed for mmcblk0rpmb" > /dev/kmsg
    fi
    for dm in $dmpts; do
        echo $ra_kb > $dm || echo "read_ahead: Failed for $dm" > /dev/kmsg
    done
}

function disable_core_ctl() {
    if [ -f /sys/devices/system/cpu/cpu0/core_ctl/enable ]; then
        echo 0 > /sys/devices/system/cpu/cpu0/core_ctl/enable || echo "core_ctl: Failed to disable cpu0" > /dev/kmsg
    else
        echo 1 > /sys/devices/system/cpu/cpu0/core_ctl/disable || echo "core_ctl: Failed to disable cpu0" > /dev/kmsg
    fi
}

function configure_memory_parameters() {
    MemTotalStr=`cat /proc/meminfo | grep MemTotal`
    MemTotal=${MemTotalStr:16:8}
    let RamSizeGB="( $MemTotal / 1048576 ) + 1"

    # Set swappiness: 80 for ≤6GB, 50 for >6GB
    if [ $RamSizeGB -le 6 ]; then
        echo 80 > /proc/sys/vm/swappiness || echo "memory: Failed to set swappiness to 80" > /dev/kmsg
    else
        echo 50 > /proc/sys/vm/swappiness || echo "memory: Failed to set swappiness to 50" > /dev/kmsg
    fi

    # Disable writeback for efficiency
    echo 1 > /proc/sys/vm/writeback_background_ratio

    # Set per-app max kgsl reclaim limits
    if [ -f /sys/class/kgsl/kgsl/page_reclaim_per_call ]; then
        echo 38400 > /sys/class/kgsl/kgsl/page_reclaim_per_call || echo "kgsl: Failed to set page_reclaim_per_call" > /dev/kmsg
    fi
    if [ -f /sys/class/kgsl/kgsl/max_reclaim_limit ]; then
        echo 25600 > /sys/class/kgsl/kgsl/max_reclaim_limit || echo "kgsl: Failed to set max_reclaim_limit" > /dev/kmsg
    fi

    # Disable periodic kcompactd wakeups (no THP)
    echo 0 > /proc/sys/vm/compaction_proactiveness

    # Disable THP to prevent min_free_kbytes reset
    if [ -f /sys/kernel/mm/transparent_hugepage/enabled ]; then
        echo never > /sys/kernel/mm/transparent_hugepage/enabled || echo "memory: Failed to disable THP" > /dev/kmsg
    fi

    # Set min_free_kbytes based on RAM
    if [ $RamSizeGB -ge 8 ]; then
        echo 11584 > /proc/sys/vm/min_free_kbytes || echo "memory: Failed to set min_free_kbytes to 11584" > /dev/kmsg
    elif [ $RamSizeGB -ge 4 ]; then
        echo 8192 > /proc/sys/vm/min_free_kbytes || echo "memory: Failed to set min_free_kbytes to 8192" > /dev/kmsg
    elif [ $RamSizeGB -ge 2 ]; then
        echo 5792 > /proc/sys/vm/min_free_kbytes || echo "memory: Failed to set min_free_kbytes to 5792" > /dev/kmsg
    else
        echo 4096 > /proc/sys/vm/min_free_kbytes || echo "memory: Failed to set min_free_kbytes to 4096" > /dev/kmsg
    fi

    configure_zram_parameters
    configure_read_ahead_kb_values
}

function start_hbtp() {
    bootmode=`getprop ro.bootmode`
    if [ "charger" != $bootmode ]; then
        start vendor.hbtp || echo "hbtp: Failed to start vendor.hbtp" > /dev/kmsg
    fi
}

# Get SoC ID
if [ -f /sys/devices/soc0/soc_id ]; then
    soc_id=`cat /sys/devices/soc0/soc_id`
else
    soc_id=`cat /sys/devices/system/soc/soc0/id`
fi

# Configure memory parameters
configure_memory_parameters

# Configure RT parameters
long_running_rt_task_ms=1200
sched_rt_runtime_ms=$((long_running_rt_task_ms + 50))
sched_rt_runtime_us=$((sched_rt_runtime_ms * 1000))
sched_rt_period_ms=$((sched_rt_runtime_ms + 100))
sched_rt_period_us=$((sched_rt_period_ms * 1000))
if [ -d /sys/module/sched_walt_debug ]; then
    echo $long_running_rt_task_ms > /proc/sys/walt/sched_long_running_rt_task_ms || echo "RT: Failed to set long_running_rt_task_ms" > /dev/kmsg
fi
echo $sched_rt_period_us > /proc/sys/kernel/sched_rt_period_us || echo "RT: Failed to set sched_rt_period_us" > /dev/kmsg
echo $sched_rt_runtime_us > /proc/sys/kernel/sched_rt_runtime_us || echo "RT: Failed to set sched_rt_runtime_us" > /dev/kmsg

# Core control settings
disable_core_ctl
echo 2 > /sys/devices/system/cpu/cpu4/core_ctl/min_cpus || echo "core_ctl: Failed to set min_cpus" > /dev/kmsg
echo 60 > /sys/devices/system/cpu/cpu4/core_ctl/busy_up_thres || echo "core_ctl: Failed to set busy_up_thres" > /dev/kmsg
echo 40 > /sys/devices/system/cpu/cpu4/core_ctl/busy_down_thres || echo "core_ctl: Failed to set busy_down_thres" > /dev/kmsg
echo 100 > /sys/devices/system/cpu/cpu4/core_ctl/offline_delay_ms || echo "core_ctl: Failed to set offline_delay_ms" > /dev/kmsg
echo 4 > /sys/devices/system/cpu/cpu4/core_ctl/task_thres || echo "core_ctl: Failed to set task_thres" > /dev/kmsg

# Scheduler parameters
echo 65 > /proc/sys/walt/sched_downmigrate || echo "sched: Failed to set downmigrate" > /dev/kmsg
echo 71 > /proc/sys/walt/sched_upmigrate || echo "sched: Failed to set upmigrate" > /dev/kmsg
echo 100 > /proc/sys/walt/sched_group_upmigrate || echo "sched: Failed to set group_upmigrate" > /dev/kmsg
echo 85 > /proc/sys/walt/sched_group_downmigrate || echo "sched: Failed to set group_downmigrate" > /dev/kmsg
echo 1 > /proc/sys/walt/sched_walt_rotate_big_tasks || echo "sched: Failed to set rotate_big_tasks" > /dev/kmsg
echo 400000000 > /proc/sys/walt/sched_coloc_downmigrate_ns || echo "sched: Failed to set coloc_downmigrate_ns" > /dev/kmsg
echo 39000000 39000000 39000000 39000000 39000000 39000000 39000000 39000000 > /proc/sys/walt/sched_coloc_busy_hyst_cpu_ns || echo "sched: Failed to set coloc_busy_hyst_cpu_ns" > /dev/kmsg
echo 248 > /proc/sys/walt/sched_coloc_busy_hysteresis_enable_cpus || echo "sched: Failed to set coloc_busy_hysteresis_enable_cpus" > /dev/kmsg
echo 10 10 10 10 10 10 10 10 > /proc/sys/walt/sched_coloc_busy_hyst_cpu_busy_pct || echo "sched: Failed to set coloc_busy_hyst_cpu_busy_pct" > /dev/kmsg
echo 8500000 8500000 8500000 8500000 8500000 8500000 8500000 8500000 > /proc/sys/walt/sched_util_busy_hyst_cpu_ns || echo "sched: Failed to set util_busy_hyst_cpu_ns" > /dev/kmsg
echo 255 > /proc/sys/walt/sched_util_busy_hysteresis_enable_cpus || echo "sched: Failed to set util_busy_hysteresis_enable_cpus" > /dev/kmsg
echo 1 1 1 1 1 1 1 1 > /proc/sys/walt/sched_util_busy_hyst_cpu_util || echo "sched: Failed to set util_busy_hyst_cpu_util" > /dev/kmsg
echo 40 > /proc/sys/walt/sched_cluster_util_thres_pct || echo "sched: Failed to set cluster_util_thres_pct" > /dev/kmsg
echo 0 > /proc/sys/walt/sched_idle_enough || echo "sched: Failed to set idle_enough" > /dev/kmsg

# Early upmigrate tunables
freq_to_migrate=1228800
silver_fmax=`cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq`
silver_early_upmigrate="$((1024 * $silver_fmax / $freq_to_migrate))"
silver_early_downmigrate="$((((1024 * $silver_fmax) / (((10*$freq_to_migrate) - $silver_fmax) / 10))))"
sched_upmigrate=`cat /proc/sys/walt/sched_upmigrate`
sched_downmigrate=`cat /proc/sys/walt/sched_downmigrate`
sched_upmigrate=${sched_upmigrate:0:2}
sched_downmigrate=${sched_downmigrate:0:2}
gold_early_upmigrate="$((1024 * 100 / $sched_upmigrate))"
gold_early_downmigrate="$((1024 * 100 / $sched_downmigrate))"
echo $silver_early_downmigrate $gold_early_downmigrate > /proc/sys/walt/sched_early_downmigrate || echo "sched: Failed to set early_downmigrate" > /dev/kmsg
echo $silver_early_upmigrate $gold_early_upmigrate > /proc/sys/walt/sched_early_upmigrate || echo "sched: Failed to set early_upmigrate" > /dev/kmsg

# Low latency task boost
echo 325 > /proc/sys/walt/walt_low_latency_task_threshold || echo "sched: Failed to set low_latency_task_threshold" > /dev/kmsg

# CPUset parameters
echo 0-3 > /dev/cpuset/background/cpus || echo "cpuset: Failed to set background cpus" > /dev/kmsg
echo 0-3 > /dev/cpuset/system-background/cpus || echo "cpuset: Failed to set system-background cpus" > /dev/kmsg
echo 4-7 > /dev/cpuset/foreground/boost/cpus || echo "cpuset: Failed to set foreground/boost cpus" > /dev/kmsg
echo 0-2,4-7 > /dev/cpuset/foreground/cpus || echo "cpuset: Failed to set foreground cpus" > /dev/kmsg
echo 0-7 > /dev/cpuset/top-app/cpus || echo "cpuset: Failed to set top-app cpus" > /dev/kmsg

# Disable scheduler boost
echo 0 > /proc/sys/walt/sched_boost || echo "sched: Failed to disable boost" > /dev/kmsg

# Reset RT clamp
echo 0 > /proc/sys/kernel/sched_util_clamp_min_rt_default || echo "sched: Failed to reset util_clamp_min_rt_default" > /dev/kmsg

# Configure governor settings (silver cluster)
echo "walt" > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor || echo "governor: Failed to set walt for policy0" > /dev/kmsg
echo 0 > /sys/devices/system/cpu/cpufreq/policy0/walt/down_rate_limit_us || echo "governor: Failed to set down_rate_limit_us for policy0" > /dev/kmsg
echo 0 > /sys/devices/system/cpu/cpufreq/policy0/walt/up_rate_limit_us || echo "governor: Failed to set up_rate_limit_us for policy0" > /dev/kmsg
echo 1516800 > /sys/devices/system/cpu/cpufreq/policy0/walt/hispeed_freq || echo "governor: Failed to set hispeed_freq for policy0" > /dev/kmsg
echo 691200 > /sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq || echo "governor: Failed to set scaling_min_freq for policy0" > /dev/kmsg
echo 1 > /sys/devices/system/cpu/cpufreq/policy0/walt/pl || echo "governor: Failed to set pl for policy0" > /dev/kmsg
echo 0 > /sys/devices/system/cpu/cpufreq/policy0/walt/rtg_boost_freq || echo "governor: Failed to set rtg_boost_freq for policy0" > /dev/kmsg

# Configure governor settings (gold cluster)
echo "walt" > /sys/devices/system/cpu/cpufreq/policy4/scaling_governor || echo "governor: Failed to set walt for policy4" > /dev/kmsg
echo 0 > /sys/devices/system/cpu/cpufreq/policy4/walt/down_rate_limit_us || echo "governor: Failed to set down_rate_limit_us for policy4" > /dev/kmsg
echo 0 > /sys/devices/system/cpu/cpufreq/policy4/walt/up_rate_limit_us || echo "governor: Failed to set up_rate_limit_us for policy4" > /dev/kmsg
echo 1344000 > /sys/devices/system/cpu/cpufreq/policy4/walt/hispeed_freq || echo "governor: Failed to set hispeed_freq for policy4" > /dev/kmsg
echo 1056000 > /sys/devices/system/cpu/cpufreq/policy4/scaling_min_freq || echo "governor: Failed to set scaling_min_freq for policy4" > /dev/kmsg
echo 1 > /sys/devices/system/cpu/cpufreq/policy4/walt/pl || echo "governor: Failed to set pl for policy4" > /dev/kmsg
echo 0 > /sys/devices/system/cpu/cpufreq/policy4/walt/rtg_boost_freq || echo "governor: Failed to set rtg_boost_freq for policy4" > /dev/kmsg

# Configure bus-dcvs
bus_dcvs="/sys/devices/system/cpu/bus_dcvs"
for device in $bus_dcvs/*; do
    cat $device/hw_min_freq > $device/boost_freq || echo "bus-dcvs: Failed to set boost_freq for $device" > /dev/kmsg
done
for ddrbw in $bus_dcvs/DDR/*bwmon-ddr; do
    echo "762 2086 2929 3879 5931 6881 7980" > $ddrbw/mbps_zones || echo "bus-dcvs: Failed to set mbps_zones for $ddrbw" > /dev/kmsg
    echo 4 > $ddrbw/sample_ms || echo "bus-dcvs: Failed to set sample_ms for $ddrbw" > /dev/kmsg
    echo 85 > $ddrbw/io_percent || echo "bus-dcvs: Failed to set io_percent for $ddrbw" > /dev/kmsg
    echo 20 > $ddrbw/hist_memory || echo "bus-dcvs: Failed to set hist_memory for $ddrbw" > /dev/kmsg
    echo 0 > $ddrbw/hyst_length || echo "bus-dcvs: Failed to set hyst_length for $ddrbw" > /dev/kmsg
    echo 80 > $ddrbw/down_thres || echo "bus-dcvs: Failed to set down_thres for $ddrbw" > /dev/kmsg
    echo 0 > $ddrbw/guard_band_mbps || echo "bus-dcvs: Failed to set guard_band_mbps for $ddrbw" > /dev/kmsg
    echo 250 > $ddrbw/up_scale || echo "bus-dcvs: Failed to set up_scale for $ddrbw" > /dev/kmsg
    echo 1600 > $ddrbw/idle_mbps || echo "bus-dcvs: Failed to set idle_mbps for $ddrbw" > /dev/kmsg
    echo 2092000 > $ddrbw/max_freq || echo "bus-dcvs: Failed to set max_freq for $ddrbw" > /dev/kmsg
done

# Power settings
echo s2idle > /sys/power/mem_sleep || echo "power: Failed to set s2idle" > /dev/kmsg
echo N > /sys/devices/system/cpu/qcom_lpm/parameters/sleep_disabled || echo "power: Failed to set sleep_disabled" > /dev/kmsg

# Set image version
if [ -f /sys/devices/soc0/select_image ]; then
    image_version="10:"
    image_version+=`getprop ro.build.id`
    image_version+=":"
    image_version+=`getprop ro.build.version.incremental`
    image_variant=`getprop ro.product.name`
    image_variant+="-"
    image_variant+=`getprop ro.build.type`
    oem_version=`getprop ro.build.version.codename`
    echo 10 > /sys/devices/soc0/select_image || echo "image: Failed to set select_image" > /dev/kmsg
    echo $image_version > /sys/devices/soc0/image_version || echo "image: Failed to set image_version" > /dev/kmsg
    echo $image_variant > /sys/devices/soc0/image_variant || echo "image: Failed to set image_variant" > /dev/kmsg
    echo $oem_version > /sys/devices/soc0/image_crm_version || echo "image: Failed to set image_crm_version" > /dev/kmsg
fi

# Configure console log level
console_config=`getprop persist.vendor.console.silent.config`
case "$console_config" in
    "1")
        echo 0 > /proc/sys/kernel/printk || echo "console: Failed to set printk to 0" > /dev/kmsg
        ;;
    *)
        echo 4 > /proc/sys/kernel/printk || echo "console: Failed to set printk to 4" > /dev/kmsg
        ;;
esac

# Post-setup services
setprop vendor.post_boot.parsed 1
start_hbtp