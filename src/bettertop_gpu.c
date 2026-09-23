/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE

#include "bettertop_gpu_protocol.h"
#include "nvtop/extract_gpuinfo_common.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

extern struct gpu_vendor gpu_vendor_nvidia;
extern bool nvtop_nvidia_get_identity(struct gpu_info *, char *, size_t, char *, size_t);
extern bool nvtop_nvidia_processes_truncated(struct gpu_info *);
extern bool nvtop_nvidia_devices_truncated(void);
extern int nvtop_nvidia_last_status(void);
extern void nvtop_nvidia_set_diagnostics(bool);

static uint64_t sequence;
static bool diagnostics_enabled;

static bool set_parent_death_signal(void) {
#if defined(__linux__)
  pid_t parent = getppid();
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent)
    return false;
#endif
  return true;
}

static bool detach_stdio(void) {
  int nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (nullfd < 0)
    return false;
  bool ok = dup2(nullfd, STDIN_FILENO) >= 0 && dup2(nullfd, STDERR_FILENO) >= 0;
  if (nullfd > STDERR_FILENO)
    close(nullfd);
  return ok;
}

static bool frame_size(uint32_t devices, uint32_t processes, uint32_t *size) {
  uint64_t total;
  if (devices > BETTERTOP_GPU_MAX_DEVICES || processes > BETTERTOP_GPU_MAX_PROCESSES)
    return false;
  total = sizeof(bettertop_gpu_frame_header) + (uint64_t)devices * sizeof(bettertop_gpu_device_record) +
          (uint64_t)processes * sizeof(bettertop_gpu_process_record);
  if (total > BETTERTOP_GPU_MAX_FRAME_BYTES)
    return false;
  *size = (uint32_t)total;
  return true;
}

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void copy_clean_string(char *dst, size_t dst_size, const char *src, size_t src_size) {
  size_t i;
  if (!dst_size)
    return;
  for (i = 0; i + 1 < dst_size && i < src_size && src[i]; ++i) {
    unsigned char c = (unsigned char)src[i];
    dst[i] = c >= 0x20 && c <= 0x7e ? (char)c : '?';
  }
  dst[i] = '\0';
}

static bool write_all(int fd, const void *data, size_t len) {
  const unsigned char *p = data;
  while (len) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    p += (size_t)n;
    len -= (size_t)n;
  }
  return true;
}

static bettertop_gpu_frame_header make_header(uint32_t status, uint32_t detail, uint32_t devices,
                                              uint32_t processes, uint32_t flags) {
  bettertop_gpu_frame_header header = {0};
  memcpy(header.magic, BETTERTOP_GPU_MAGIC, sizeof(header.magic));
  header.version = BETTERTOP_GPU_PROTOCOL_VERSION;
  header.header_size = sizeof(header);
  frame_size(devices, processes, &header.frame_size);
  header.status = status;
  header.sequence = ++sequence;
  header.monotonic_sample_ns = monotonic_ns();
  header.device_count = devices;
  header.process_count = processes;
  header.flags = flags;
  header.status_detail = detail;
  return header;
}

static bool write_frame(uint32_t status, uint32_t detail, uint32_t device_count, uint32_t process_count,
                        uint32_t flags, const bettertop_gpu_device_record *devices,
                        const bettertop_gpu_process_record *processes) {
  bettertop_gpu_frame_header header;
  if (!frame_size(device_count, process_count, &header.frame_size))
    return false;
  header = make_header(status, detail, device_count, process_count, flags);
  return write_all(STDOUT_FILENO, &header, sizeof(header)) &&
         write_all(STDOUT_FILENO, devices, (size_t)device_count * sizeof(*devices)) &&
         write_all(STDOUT_FILENO, processes, (size_t)process_count * sizeof(*processes));
}

static bool field_valid(const unsigned char *valid, unsigned bit) {
  return (valid[bit / CHAR_BIT] & (1u << (bit % CHAR_BIT))) != 0;
}

static void fill_device_record(bettertop_gpu_device_record *record, struct gpu_info *gpu, uint32_t index) {
  const struct gpuinfo_dynamic_info *dynamic = &gpu->dynamic_info;
  char bus_id[32] = {0};
  char uuid[80] = {0};

  memset(record, 0, sizeof(*record));
  record->display_index = index;
  if (GPUINFO_DYNAMIC_FIELD_VALID(dynamic, gpu_util_rate) && dynamic->gpu_util_rate <= 100) {
    record->gpu_util_pct = dynamic->gpu_util_rate;
    record->valid_fields |= BETTERTOP_GPU_DEVICE_GPU_UTIL;
  }
  if (GPUINFO_DYNAMIC_FIELD_VALID(dynamic, mem_util_rate) && dynamic->mem_util_rate <= 100) {
    record->memory_util_pct = dynamic->mem_util_rate;
    record->valid_fields |= BETTERTOP_GPU_DEVICE_MEMORY_UTIL;
  }
  if (GPUINFO_DYNAMIC_FIELD_VALID(dynamic, total_memory) && dynamic->total_memory != ULLONG_MAX) {
    record->memory_total_bytes = dynamic->total_memory;
    record->valid_fields |= BETTERTOP_GPU_DEVICE_MEMORY_TOTAL;
  }
  if (GPUINFO_DYNAMIC_FIELD_VALID(dynamic, used_memory) && dynamic->used_memory != ULLONG_MAX) {
    record->memory_used_bytes = dynamic->used_memory;
    record->valid_fields |= BETTERTOP_GPU_DEVICE_MEMORY_USED;
  }
  if (GPUINFO_DYNAMIC_FIELD_VALID(dynamic, gpu_temp) && dynamic->gpu_temp != UINT_MAX) {
    record->temperature_c = dynamic->gpu_temp;
    record->valid_fields |= BETTERTOP_GPU_DEVICE_TEMPERATURE;
  }
  if (GPUINFO_DYNAMIC_FIELD_VALID(dynamic, power_draw) && dynamic->power_draw != UINT_MAX) {
    record->power_mw = dynamic->power_draw;
    record->valid_fields |= BETTERTOP_GPU_DEVICE_POWER_DRAW;
  }
  if (GPUINFO_DYNAMIC_FIELD_VALID(dynamic, power_draw_max) && dynamic->power_draw_max != UINT_MAX) {
    record->power_limit_mw = dynamic->power_draw_max;
    record->valid_fields |= BETTERTOP_GPU_DEVICE_POWER_LIMIT;
  }
  if (GPUINFO_DYNAMIC_FIELD_VALID(dynamic, pcie_tx) && dynamic->pcie_tx != UINT_MAX) {
    record->pcie_tx_bytes_s = (uint64_t)dynamic->pcie_tx * 1024u;
    record->valid_fields |= BETTERTOP_GPU_DEVICE_PCIE_TX;
  }
  if (GPUINFO_DYNAMIC_FIELD_VALID(dynamic, pcie_rx) && dynamic->pcie_rx != UINT_MAX) {
    record->pcie_rx_bytes_s = (uint64_t)dynamic->pcie_rx * 1024u;
    record->valid_fields |= BETTERTOP_GPU_DEVICE_PCIE_RX;
  }
  /* nvtop stores NVLink rates as KiB/s; safe profile never sets these bits. */
  struct nvlink_info nvlink;
  if (diagnostics_enabled && nvtop_get_nvlink_info(gpu, &nvlink) && nvlink.has_throughput) {
    record->nvlink_tx_bytes_s = nvlink.aggregate_tx * 1024u;
    record->nvlink_rx_bytes_s = nvlink.aggregate_rx * 1024u;
    record->valid_fields |= BETTERTOP_GPU_DEVICE_NVLINK_TX | BETTERTOP_GPU_DEVICE_NVLINK_RX;
  }
  if (nvtop_nvidia_get_identity(gpu, bus_id, sizeof(bus_id), uuid, sizeof(uuid))) {
    copy_clean_string(record->pci_bus_id, sizeof(record->pci_bus_id), bus_id, sizeof(bus_id));
    copy_clean_string(record->uuid, sizeof(record->uuid), uuid, sizeof(uuid));
  }
  if (field_valid(gpu->static_info.valid, gpuinfo_device_name_valid))
    copy_clean_string(record->name, sizeof(record->name), gpu->static_info.device_name,
                      sizeof(gpu->static_info.device_name));
}

static uint32_t process_kind(enum gpu_process_type type) {
  switch (type) {
  case gpu_process_graphical:
    return BETTERTOP_GPU_PROCESS_GRAPHICS;
  case gpu_process_compute:
    return BETTERTOP_GPU_PROCESS_COMPUTE;
  case gpu_process_graphical_compute:
    return BETTERTOP_GPU_PROCESS_GRAPHICS | BETTERTOP_GPU_PROCESS_COMPUTE;
  default:
    return 0;
  }
}

static uint32_t append_processes(struct gpu_info *gpu, uint32_t device_index,
                                 bettertop_gpu_process_record *records, uint32_t count) {
  uint32_t added = 0;
  for (uint32_t i = 0; i < gpu->processes_count && count + added < BETTERTOP_GPU_MAX_PROCESSES; ++i) {
    const struct gpu_process *source = &gpu->processes[i];
    if (source->pid <= 0)
      continue;
    bettertop_gpu_process_record *record = &records[count + added];
    memset(record, 0, sizeof(*record));
    record->device_record_index = device_index;
    record->pid = (uint32_t)source->pid;
    record->kind_flags = process_kind(source->type);
    if (GPUINFO_PROCESS_FIELD_VALID(source, gpu_memory_usage) && source->gpu_memory_usage != ULLONG_MAX) {
      record->vram_bytes = source->gpu_memory_usage;
      record->valid_fields |= BETTERTOP_GPU_PROCESS_MEMORY;
    }
    if (GPUINFO_PROCESS_FIELD_VALID(source, gpu_usage) && source->gpu_usage <= 100) {
      record->gpu_util_pct = source->gpu_usage;
      record->valid_fields |= BETTERTOP_GPU_PROCESS_GPU_UTIL;
    }
    ++added;
  }
  return count + added;
}

static bool collect(struct list_head *devices, bool static_info_loaded) {
  bettertop_gpu_device_record device_records[BETTERTOP_GPU_MAX_DEVICES] = {{0}};
  bettertop_gpu_process_record *process_records = calloc(BETTERTOP_GPU_MAX_PROCESSES, sizeof(*process_records));
  if (!process_records)
    return false;

  uint32_t device_count = 0, process_count = 0, flags = 0;
  struct gpu_info *gpu;
  list_for_each_entry(gpu, devices, list) {
    if (device_count == BETTERTOP_GPU_MAX_DEVICES) {
      flags |= BETTERTOP_GPU_TRUNCATED_DEVICES;
      break;
    }
    if (!static_info_loaded)
      gpu->vendor->populate_static_info(gpu);
    gpu->vendor->refresh_dynamic_info(gpu);
    fill_device_record(&device_records[device_count], gpu, device_count);
    gpu->vendor->refresh_running_processes(gpu);
    uint32_t before = process_count;
    process_count = append_processes(gpu, device_count, process_records, process_count);
    if (nvtop_nvidia_processes_truncated(gpu) || process_count - before < gpu->processes_count)
      flags |= BETTERTOP_GPU_TRUNCATED_PROCESSES;
    free(gpu->processes);
    gpu->processes = NULL;
    gpu->processes_count = 0;
    gpu->processes_array_size = 0;
    ++device_count;
  }
  if (nvtop_nvidia_devices_truncated())
    flags |= BETTERTOP_GPU_TRUNCATED_DEVICES;

  bool result = write_frame(BETTERTOP_GPU_STATUS_OK, 0, device_count, process_count, flags, device_records,
                            process_records);
  free(process_records);
  return result;
}

static int protocol_self_check(void) {
  uint32_t size = 0;
  if (sizeof(bettertop_gpu_frame_header) != 48 || sizeof(bettertop_gpu_device_record) != 336 ||
      sizeof(bettertop_gpu_process_record) != 40 ||
      !frame_size(BETTERTOP_GPU_MAX_DEVICES, BETTERTOP_GPU_MAX_PROCESSES, &size) ||
      size > BETTERTOP_GPU_MAX_FRAME_BYTES || frame_size(BETTERTOP_GPU_MAX_DEVICES + 1, 0, &size) ||
      frame_size(0, BETTERTOP_GPU_MAX_PROCESSES + 1, &size)) {
    fprintf(stderr, "bettertop-gpu protocol self-check failed\n");
    return 1;
  }
  fprintf(stderr, "bettertop-gpu protocol v%u: %u-byte maximum frame\n", BETTERTOP_GPU_PROTOCOL_VERSION,
          size);
  return 0;
}

static bool sleep_interval(unsigned interval_ms) {
  struct timespec request = {.tv_sec = interval_ms / 1000,
                             .tv_nsec = (long)(interval_ms % 1000) * 1000000L};
  struct timespec remaining;
  while (nanosleep(&request, &remaining) != 0) {
    if (errno != EINTR)
      return false;
    request = remaining;
  }
  return true;
}

static bool parse_interval(const char *text, unsigned *interval_ms) {
  if (!text[0])
    return false;
  for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
    if (*p < '0' || *p > '9')
      return false;
  char *end = NULL;
  errno = 0;
  unsigned long value = strtoul(text, &end, 10);
  if (errno || !text[0] || !end || *end || value < 250 || value > 30000)
    return false;
  *interval_ms = (unsigned)value;
  return true;
}

static void usage(void) {
  fprintf(stderr, "usage: bettertop-gpu [--interval-ms 250..30000] [--diagnostics] [--once|--self-check]\n");
}

int main(int argc, char **argv) {
  if (!set_parent_death_signal())
    return 1;
  unsigned interval_ms = 1000;
  bool diagnostics = false, once = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc) {
      if (!parse_interval(argv[++i], &interval_ms)) {
        usage();
        return 2;
      }
    } else if (!strcmp(argv[i], "--diagnostics")) {
      diagnostics = true;
    } else if (!strcmp(argv[i], "--once")) {
      once = true;
    } else if (!strcmp(argv[i], "--self-check")) {
      return protocol_self_check();
    } else {
      usage();
      return 2;
    }
  }

  if (isatty(STDOUT_FILENO) || !detach_stdio())
    return 1;
  nvtop_nvidia_set_diagnostics(diagnostics);
  diagnostics_enabled = diagnostics;
  LIST_HEAD(devices);
  if (!gpu_vendor_nvidia.init()) {
    return write_frame(BETTERTOP_GPU_STATUS_UNAVAILABLE, (uint32_t)nvtop_nvidia_last_status(), 0, 0, 0, NULL,
                       NULL)
               ? 0
               : 1;
  }

  unsigned total_devices = 0;
  if (!gpu_vendor_nvidia.get_device_handles(&devices, &total_devices)) {
    write_frame(BETTERTOP_GPU_STATUS_ERROR, (uint32_t)nvtop_nvidia_last_status(), 0, 0, 0, NULL, NULL);
    gpu_vendor_nvidia.shutdown();
    return 1;
  }
  (void)total_devices;

  bool static_info_loaded = false;
  for (;;) {
    if (!collect(&devices, static_info_loaded)) {
      gpu_vendor_nvidia.shutdown();
      return 1;
    }
    static_info_loaded = true;
    if (once)
      break;
    if (!sleep_interval(interval_ms)) {
      gpu_vendor_nvidia.shutdown();
      return 1;
    }
  }
  gpu_vendor_nvidia.shutdown();
  return 0;
}
