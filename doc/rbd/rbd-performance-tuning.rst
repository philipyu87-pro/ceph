==============================
 librbd Performance Tuning
==============================

.. index:: Ceph Block Device; Performance Tuning

This document provides performance tuning recommendations for ``librbd``-based
RBD clients, such as QEMU/KVM virtual machines configured with multiple RBD
disks. The guidance covers both Ceph configuration parameters and OS-level
settings, with additional notes for ARM aarch64 platforms (e.g., Kunpeng 920).

.. note:: The optimal settings depend on the specific workload, hardware
   configuration, and Ceph cluster topology. Always benchmark with
   representative workloads before and after applying changes.

librbd Cache Tuning
===================

The ``librbd`` user-space cache can significantly improve performance for
workloads with locality of reference or frequent small writes. Each RBD image
maintains its own independent cache instance.

Write-back Cache
----------------

For VM workloads where the guest OS sends proper flush requests (Linux kernel
>= 2.6.32), write-back caching is recommended::

    [client]
    rbd_cache = true
    rbd_cache_policy = writeback
    rbd_cache_size = 67108864          # 64 MB (default: 32 MB)
    rbd_cache_max_dirty = 50331648     # 48 MB (default: 24 MB)
    rbd_cache_target_dirty = 33554432  # 32 MB (default: 16 MB)
    rbd_cache_max_dirty_age = 2        # seconds (default: 1)

Increasing ``rbd_cache_size`` allows more data to be cached per image.
``rbd_cache_max_dirty`` controls the threshold at which dirty data begins to be
flushed to the cluster; setting it closer to ``rbd_cache_size`` allows more
coalescing of writes before flushing.

.. important:: When using QEMU, you must set ``cache=writeback`` on the drive
   option to enable write-back mode. Without it, QEMU will not send flush
   requests to ``librbd`` and data loss may occur on unexpected termination.
   See `QEMU and Block Devices`_ for details.

Read-ahead / Prefetch
---------------------

Read-ahead can improve sequential read performance, particularly during
boot or when the guest does not issue efficient reads::

    [client]
    rbd_readahead_trigger_requests = 10   # sequential reads to trigger (default: 10)
    rbd_readahead_max_bytes = 1048576     # 1 MB (default: 512 KB)
    rbd_readahead_disable_after_bytes = 0 # 0 = never disable (default: 50 MB)

.. note:: Read-ahead is automatically disabled if caching is disabled or if
   the cache policy is ``writearound``.

Persistent Write Log Cache
---------------------------

For workloads requiring both high write performance and crash consistency,
consider enabling the Persistent Write Log (PWL) cache backed by a local
SSD or PMEM device. This can reduce write latency significantly by
persisting data locally before flushing to the cluster. See
:doc:`rbd-persistent-write-log-cache` for setup details.


Image Configuration
===================

Object Size
-----------

The default object size for RBD images is 4 MB (``rbd_default_order = 22``,
i.e. 2^22 bytes). Larger object sizes can improve throughput for sequential
workloads and reduce metadata overhead, at the cost of increased latency for
small random I/Os::

    rbd_default_order = 22  # 4 MB objects (default)
    # rbd_default_order = 23  # 8 MB objects (for large sequential I/O)

This setting applies only when creating new images. Existing images retain
their configured object size.

Read from Replica Policy
------------------------

In replicated pools, reads can be distributed across replicas to improve
throughput and reduce latency::

    rbd_read_from_replica_policy = balance   # or 'localize' (default: 'default')

- ``balance``: Distributes reads across all replicas.
- ``localize``: Prefers reading from the local OSD when available.

Image Features
--------------

For VM workloads using a single writer per image, the following features should
be enabled for optimal performance:

- **exclusive-lock**: Required for object-map and fast-diff, and for the
  persistent write log cache.
- **object-map**: Speeds up operations on sparse images by tracking which
  objects exist.
- **fast-diff**: Improves snapshot diff and usage calculation speed.

Journaling should be disabled unless required for RBD mirroring, as it
introduces additional write overhead.

QoS Throttling
--------------

The default QoS settings impose no limits. If QoS limits are configured,
verify they are not unintentionally restricting performance. In particular,
check the following per-image settings::

    rbd_qos_iops_limit = 0       # 0 = unlimited (default)
    rbd_qos_bps_limit = 0        # 0 = unlimited (default)
    rbd_qos_read_iops_limit = 0  # 0 = unlimited (default)
    rbd_qos_write_iops_limit = 0 # 0 = unlimited (default)


OS and Hypervisor Tuning
========================

The following host OS settings can impact ``librbd`` performance when running
VMs.

I/O Scheduler
-------------

For NVMe-backed OSDs, the ``none`` (noop) I/O scheduler is generally
recommended on the OSD hosts to minimize unnecessary scheduling overhead::

    echo none > /sys/block/<device>/queue/scheduler

For rotational drives, ``mq-deadline`` is typically preferred.

CPU Frequency Scaling
---------------------

Disable CPU frequency scaling (set the governor to ``performance``) on both
OSD hosts and hypervisor hosts to avoid latency variance::

    cpupower frequency-set -g performance

Or for all CPUs via sysfs::

    echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

Network Tuning
--------------

For high-throughput RBD workloads, consider the following network settings on
both client (hypervisor) and OSD hosts::

    # Increase socket buffer sizes
    sysctl -w net.core.rmem_max=16777216
    sysctl -w net.core.wmem_max=16777216
    sysctl -w net.ipv4.tcp_rmem="4096 87380 16777216"
    sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216"

    # Increase network device backlog
    sysctl -w net.core.netdev_max_backlog=250000

For RDMA or high-speed networks (25 GbE+), also consider tuning the MSI-X
interrupt affinity and enabling RFS/RPS to distribute network processing
across cores.

NUMA Awareness
--------------

On multi-socket or multi-NUMA-node systems, pin QEMU/KVM processes and their
associated vCPU threads to the same NUMA node as the network adapter used for
Ceph traffic. This reduces cross-node memory access latency.

Use ``numactl`` or the libvirt ``<numatune>`` configuration::

    numactl --cpunodebind=0 --membind=0 qemu-system-aarch64 ...

Or in the libvirt domain XML:

.. code-block:: xml

    <numatune>
      <memory mode="strict" nodeset="0"/>
    </numatune>


Ceph Cluster Tuning
====================

The following cluster-side settings can also affect ``librbd`` client
performance.

OSD Settings
------------

Increase the number of OSD threads and shards for NVMe-backed OSDs::

    [osd]
    osd_op_num_shards = 8             # default: 0 (auto)
    osd_op_num_threads_per_shard = 2  # default: 0 (auto)

Recovery and backfill throttling can be adjusted to reduce impact on
foreground I/O::

    osd_recovery_max_active = 1       # default: 0 (auto)
    osd_recovery_sleep = 0.1          # seconds between recovery ops

BlueStore Tuning
----------------

For NVMe-backed OSDs using BlueStore, consider increasing the BlueStore
cache size and tuning the deferred write threshold::

    bluestore_cache_size_ssd = 3221225472   # 3 GB (default: 3 GB)
    bluestore_prefer_deferred_size_ssd = 0  # disable deferred writes for SSD

See the :doc:`/rados/configuration/bluestore-config-ref` for a full list of
BlueStore tuning options.

Pool Configuration
------------------

Ensure that RBD pools have an appropriate number of placement groups (PGs)
for the cluster size. Too few PGs may cause uneven data distribution and
reduced throughput.


ARM aarch64 / Kunpeng 920 Specific Notes
=========================================

CPU and Memory
--------------

Kunpeng 920 processors provide high core counts per socket. For librbd
workloads:

- Pin QEMU/KVM vCPU threads to physical cores on the same NUMA node as the
  network interface used for Ceph traffic (see `NUMA Awareness`_ above).
- Set the CPU governor to ``performance``::

      cpupower frequency-set -g performance

- If the kernel supports it, enable Transparent Huge Pages (THP) to reduce
  TLB misses for large working sets::

      echo always > /sys/kernel/mm/transparent_hugepage/enabled

Hardware Acceleration with UADK
-------------------------------

Kunpeng 920 processors include hardware accelerators that can offload
compression and cryptographic operations via the UADK framework.

When the Ceph cluster uses compression on RBD pools (e.g., BlueStore
compression), enabling UADK offloads the compression work from the CPU,
freeing cores for I/O processing::

    [osd]
    uadk_compressor_enabled = true

This requires a Linux kernel >= 5.9 with SVA (Shared Virtual Addressing)
support enabled. See :doc:`/radosgw/uadk-accel` for UADK build and
configuration details (the UADK setup applies to OSD hosts regardless of
whether RadosGW is used).

Kernel Parameters
-----------------

On openEuler or other aarch64 distributions with a 5.10+ kernel, the
following kernel parameters may improve I/O performance:

- **IOMMU passthrough** for reduced overhead when using hardware
  acceleration or direct device assignment::

      # In kernel boot parameters
      iommu.passthrough=1

- **Disable kernel auditing** if not required, to reduce overhead::

      audit=0

- **Increase the vm.min_free_kbytes** to avoid memory pressure stalls on
  systems with large amounts of RAM::

      sysctl -w vm.min_free_kbytes=1048576  # 1 GB for 256 GB+ systems

VIRTIO and Disk I/O
-------------------

For VMs using virtio-blk or virtio-scsi with multiple RBD disks:

- Use ``virtio-blk`` instead of ``virtio-scsi`` for lower latency on simple
  configurations.
- Set ``num-queues`` to match the number of vCPUs to enable multi-queue I/O::

      -device virtio-blk-pci,drive=driveN,num-queues=4

  Or in libvirt XML:

  .. code-block:: xml

      <driver name='qemu' type='raw' cache='writeback' io='threads' queues='4'/>

- Enable I/O thread pinning in libvirt to avoid contention::

      <iothreads>4</iothreads>
      <iothreadids>
        <iothread id='1'/>
        <iothread id='2'/>
        <iothread id='3'/>
        <iothread id='4'/>
      </iothreadids>


.. _QEMU and Block Devices: ./qemu-rbd
