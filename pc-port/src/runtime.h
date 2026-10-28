// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#define GUEST_ABI __attribute__((sysv_abi))
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t base, size, init, tls_address, tls_filesz, tls_memsz, tls_align, tls_id, flags;
} GuestModule;
// flags bit 0: the module is in the image (relocated, imports bound, TLS reserved) but its
// initializer and entry wait for sceKernelLoadStartModule. Knack II's game.elf is the only one.
void runtime_configure(unsigned char *image, uint64_t image_size, const GuestModule *modules,
                       uint64_t count, uint64_t procparam);
void runtime_tls_attach(void);
void runtime_tls_detach(void);
uintptr_t runtime_resolve(const char *identity, int data);
// ISS_TRACE_MSPACE=1: wraps the bound libc.prx sceLibcMspaceMemalign/Reallocalign to log large
// requests and failures with the calling eboot offset. Returns the wrapper, or 0 to keep the export.
uintptr_t runtime_wrap_binding(const char *identity, uintptr_t native);
// The hang report writes through Win32 only (no CRT locks a stuck thread might hold).
void runtime_report_write(HANDLE out, const char *format, ...);
void runtime_report_thread(HANDLE out, const char *name, DWORD tid, const CONTEXT *c);
void runtime_report_host_thread(HANDLE out, const char *name, DWORD tid, const CONTEXT *c);
void runtime_report_dump(HANDLE out, uint64_t address, unsigned length);
void runtime_report_sample(HANDLE out, const char *name, int sample, const CONTEXT *c);
void runtime_report_dumps(HANDLE out);
// Writes every guest thread's position (hang-report.txt: on Ctrl+C, or ISS_HANG_REPORT=<seconds>).
void runtime_report_threads(HANDLE out, int resume);
int runtime_emulate_tls(EXCEPTION_POINTERS *exception);
uint64_t runtime_tls_fault_count(void);
uintptr_t sync_resolve(const char *identity);
uintptr_t thread_resolve(const char *identity);
uintptr_t time_resolve(const char *identity);
uintptr_t file_resolve(const char *identity);
uintptr_t service_resolve(const char *identity);
uintptr_t dialog_resolve(const char *identity);
uintptr_t np_resolve(const char *identity);
uintptr_t network_resolve(const char *identity);
uintptr_t save_resolve(const char *identity);
uintptr_t trophy_resolve(const char *identity);
uintptr_t pad_resolve(const char *identity);
uintptr_t audio_resolve(const char *identity);
uintptr_t kernel_sync_resolve(const char *identity);
uintptr_t http_resolve(const char *identity);
const char *runtime_user_root(void);
const char *runtime_save_root(void);
// The save data dialog of iss-gpu.dll (gpu/iss_dialog.h), when the DLL has it.
int gpu_dialog_available(void);
int gpu_dialog_open(const void *dialog);
int gpu_dialog_poll(void *result);
void gpu_dialog_close(void);
int gpu_dialog_active(void);
int runtime_file_mount(unsigned slot,const char *guest,const char *host,int readonly);
// Maps a PS4 path (/app0/..., /temp0/...) to a host path; 0 or a PS4 errno.
int runtime_file_translate(const char *guest,char *host,size_t size,int write);
uintptr_t libc_resolve(const char *identity);
uintptr_t libc_resolve_data(const char *identity);
uintptr_t offline_resolve(const char *identity);
void runtime_file_unmount(unsigned slot);
void *memory_allocate_image(uint64_t size);
void memory_register_image(uint64_t offset,uint64_t size,uint32_t flags);
uint8_t *memory_backing(uint64_t address,uint64_t size);
uint64_t memory_clamp(uint64_t address,uint64_t size);
void memory_visit(void (*visit)(uint64_t,uint64_t,uint8_t *));
uint64_t memory_mapping_epoch(void);
uint64_t memory_mapping_lock_shared(void);
void memory_mapping_unlock_shared(void);
int memory_gpu_protect(uint64_t address,uint64_t size,uint32_t permissions);
void memory_set_map_hook(void (*hook)(uint64_t,uint64_t,uint8_t *));
void memory_set_unmap_hook(void (*hook)(uint64_t,uint64_t));
uint64_t runtime_tsc_frequency(void);
uint64_t runtime_process_time_us(void);
int gpu_start(uint32_t sdk_version,uint64_t image_base);
uintptr_t gpu_resolve(const char *identity);
int gpu_handle_fault(EXCEPTION_POINTERS *exception);
int gpu_readback(uint64_t address,uint64_t size);
int gpu_invalidate(uint64_t address,uint64_t size);
int gpu_feedback_identity_available(void);
void gpu_file_opened(const char *guest_path);
void gpu_feedback_identity(uint64_t owner_object,uint64_t shader_object,uint64_t record_address);
int native_install_hooks(unsigned char *image,uint64_t size);
const char *runtime_api_name(const char *identity);
void runtime_set_thread_handle(void *handle);
int32_t *runtime_errno(void);
uint64_t GUEST_ABI memory_direct_size(void);
int32_t GUEST_ABI memory_allocate_direct(int64_t start, int64_t end, uint64_t length,
                                        uint64_t alignment, int32_t type, int64_t *result);
int32_t GUEST_ABI memory_map_direct(void **address, uint64_t length, int32_t prot,
                                   int32_t flags, int64_t physical, uint64_t alignment);
int32_t GUEST_ABI memory_map_flexible(void **address, uint64_t length, int32_t prot, int32_t flags,
                                     const char *name);
int32_t GUEST_ABI memory_protect(const void *address, uint64_t length, int32_t prot);
int32_t GUEST_ABI memory_query_protection(void *address, void **start, void **end, uint32_t *prot);
int32_t GUEST_ABI memory_release_direct(int64_t start, uint64_t length);
int32_t GUEST_ABI memory_unmap(void *address, uint64_t length);
// SceKernelBatchMapEntry arrays; batch_map is batch_map2 with the fixed-address flag.
int32_t GUEST_ABI memory_batch_map2(void *entries, int32_t count, int32_t *done, int32_t flags);
int32_t GUEST_ABI memory_batch_map(void *entries, int32_t count, int32_t *done);
// The bundled modules linked after the eboot (libc.prx, Fios2); 0 when the runtime libc stands in.
uint64_t runtime_bundled_module_count(void);
// The eboot's process parameters (sceProcParam), once runtime_configure has run.
const unsigned char *runtime_process_param(void);
uintptr_t kernel_resolve(const char *identity);
uintptr_t kernel_resolve_data(const char *identity);
// The second stage of a CUSA08014 boot. Returns a positive module handle, or a negative PS4 error.
int32_t runtime_start_deferred_module(uint64_t argc, const void *argv, int32_t *result);
uintptr_t rtc_resolve(const char *identity);
uintptr_t audio3d_resolve(const char *identity);
#ifdef __cplusplus
}
#endif
