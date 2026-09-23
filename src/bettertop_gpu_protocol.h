/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BETTERTOP_GPU_PROTOCOL_H
#define BETTERTOP_GPU_PROTOCOL_H

#include <stdint.h>

#define BETTERTOP_GPU_MAGIC "BTGP"
#define BETTERTOP_GPU_PROTOCOL_VERSION 1u
#define BETTERTOP_GPU_MAX_FRAME_BYTES (1024u * 1024u)
#define BETTERTOP_GPU_MAX_DEVICES 64u
#define BETTERTOP_GPU_MAX_PROCESSES 16384u

enum bettertop_gpu_status {
  BETTERTOP_GPU_STATUS_OK = 0,
  BETTERTOP_GPU_STATUS_UNAVAILABLE = 1,
  BETTERTOP_GPU_STATUS_ERROR = 2,
};

enum bettertop_gpu_frame_flags {
  BETTERTOP_GPU_TRUNCATED_DEVICES = 1u << 0,
  BETTERTOP_GPU_TRUNCATED_PROCESSES = 1u << 1,
};

enum bettertop_gpu_device_fields {
  BETTERTOP_GPU_DEVICE_GPU_UTIL = 1ull << 0,
  BETTERTOP_GPU_DEVICE_MEMORY_UTIL = 1ull << 1,
  BETTERTOP_GPU_DEVICE_MEMORY_TOTAL = 1ull << 2,
  BETTERTOP_GPU_DEVICE_MEMORY_USED = 1ull << 3,
  BETTERTOP_GPU_DEVICE_TEMPERATURE = 1ull << 4,
  BETTERTOP_GPU_DEVICE_POWER_DRAW = 1ull << 5,
  BETTERTOP_GPU_DEVICE_POWER_LIMIT = 1ull << 6,
  BETTERTOP_GPU_DEVICE_PCIE_TX = 1ull << 7,
  BETTERTOP_GPU_DEVICE_PCIE_RX = 1ull << 8,
  BETTERTOP_GPU_DEVICE_NVLINK_TX = 1ull << 9,
  BETTERTOP_GPU_DEVICE_NVLINK_RX = 1ull << 10,
};

enum bettertop_gpu_process_fields {
  BETTERTOP_GPU_PROCESS_MEMORY = 1ull << 0,
  BETTERTOP_GPU_PROCESS_GPU_UTIL = 1ull << 1,
};

enum bettertop_gpu_process_kind {
  BETTERTOP_GPU_PROCESS_COMPUTE = 1u << 0,
  BETTERTOP_GPU_PROCESS_GRAPHICS = 1u << 1,
};

/* Records use fixed-width fields and explicit padding. They are exchanged only
 * between same-host binaries over a pipe, never dumped from nvtop structs. */
typedef struct bettertop_gpu_frame_header {
  char magic[4];
  uint16_t version;
  uint16_t header_size;
  uint32_t frame_size;
  uint32_t status;
  uint64_t sequence;
  uint64_t monotonic_sample_ns;
  uint32_t device_count;
  uint32_t process_count;
  uint32_t flags;
  uint32_t status_detail;
} bettertop_gpu_frame_header;

typedef struct bettertop_gpu_device_record {
  uint32_t display_index;
  uint32_t reserved;
  uint64_t valid_fields;
  uint64_t memory_total_bytes;
  uint64_t memory_used_bytes;
  uint64_t power_mw;
  uint64_t power_limit_mw;
  uint64_t pcie_tx_bytes_s;
  uint64_t pcie_rx_bytes_s;
  uint64_t nvlink_tx_bytes_s;
  uint64_t nvlink_rx_bytes_s;
  uint32_t gpu_util_pct;
  uint32_t memory_util_pct;
  uint32_t temperature_c;
  uint32_t reserved2;
  char pci_bus_id[32];
  char uuid[80];
  char name[128];
} bettertop_gpu_device_record;

typedef struct bettertop_gpu_process_record {
  uint32_t device_record_index;
  uint32_t pid;
  uint32_t kind_flags;
  uint32_t reserved;
  uint64_t valid_fields;
  uint64_t vram_bytes;
  uint32_t gpu_util_pct;
  uint32_t reserved2;
} bettertop_gpu_process_record;

#if defined(__cplusplus)
static_assert(sizeof(bettertop_gpu_frame_header) == 48, "GPU protocol header layout changed");
static_assert(sizeof(bettertop_gpu_device_record) == 336, "GPU device record layout changed");
static_assert(sizeof(bettertop_gpu_process_record) == 40, "GPU process record layout changed");
#else
_Static_assert(sizeof(bettertop_gpu_frame_header) == 48, "GPU protocol header layout changed");
_Static_assert(sizeof(bettertop_gpu_device_record) == 336, "GPU device record layout changed");
_Static_assert(sizeof(bettertop_gpu_process_record) == 40, "GPU process record layout changed");
#endif

#define BETTERTOP_GPU_MAX_WIRE_BYTES \
  (sizeof(bettertop_gpu_frame_header) + BETTERTOP_GPU_MAX_DEVICES * sizeof(bettertop_gpu_device_record) + \
   BETTERTOP_GPU_MAX_PROCESSES * sizeof(bettertop_gpu_process_record))

#endif
