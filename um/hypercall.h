#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t hv_vmcall(uint64_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3);

typedef enum _hv_vmcall_code {
    hv_vmcall_read_gva = 0x100,
    hv_vmcall_write_gva = 0x101,
    hv_vmcall_enable_cr3_xor = 0x102,
    hv_vmcall_disable_cr3_xor = 0x103,
    hv_vmcall_install_shadow_hook = 0x110,
    hv_vmcall_clear_shadow_hook = 0x111,
    hv_vmcall_stealth_enable = 0x200,
    hv_vmcall_stealth_disable = 0x201,
    hv_vmcall_last_mailbox = 0x210,
    hv_vmcall_send_mailbox = 0x211,
    hv_vmcall_translate_gva_to_gpa = 0x220,
    hv_vmcall_translate_gva_to_hpa = 0x221,
    hv_vmcall_translate_gpa_to_hpa = 0x222,
    hv_vmcall_query_current_process_base = 0x320,
    hv_vmcall_query_process_base = 0x321,
    hv_vmcall_query_process_dirbase = 0x322,
    hv_vmcall_enable_syscall_hook = 0x300,
    hv_vmcall_disable_syscall_hook = 0x301,
    hv_vmcall_attach_process = 0x500,
    hv_vmcall_install_npt_hook = 0x501,
    hv_vmcall_remove_npt_hook = 0x502,
    hv_vmcall_inject_dll = 0x503,
    hv_vmcall_remove_dll = 0x504,
    hv_vmcall_query_cr3_changes = 0x505,
    hv_vmcall_query_anticheat_status = 0x506,
    hv_vmcall_reinject_all_cr3 = 0x507,
} hv_vmcall_code;

static inline uint64_t hv_query_current_process_base(void) {
    return hv_vmcall(hv_vmcall_query_current_process_base, 0, 0, 0);
}

static inline uint64_t hv_query_process_base(uint64_t pid) {
    return hv_vmcall(hv_vmcall_query_process_base, pid, 0, 0);
}

static inline uint64_t hv_query_process_dirbase(uint64_t pid) {
    return hv_vmcall(hv_vmcall_query_process_dirbase, pid, 0, 0);
}

static inline uint64_t hv_translate_gva_to_hpa(uint64_t gva) {
    return hv_vmcall(hv_vmcall_translate_gva_to_hpa, gva, 0, 0);
}

static inline uint64_t hv_attach_process(uint64_t pid, uint64_t image_base) {
    return hv_vmcall(hv_vmcall_attach_process, pid, image_base, 0);
}

static inline uint64_t hv_install_npt_hook(uint64_t target_gva, uint64_t hook_bytes_gva, uint64_t hook_size) {
    return hv_vmcall(hv_vmcall_install_npt_hook, target_gva, hook_bytes_gva, hook_size);
}

static inline uint64_t hv_remove_npt_hook(void) {
    return hv_vmcall(hv_vmcall_remove_npt_hook, 0, 0, 0);
}

static inline uint64_t hv_inject_dll(void) {
    return hv_vmcall(hv_vmcall_inject_dll, 0, 0, 0);
}

static inline uint64_t hv_remove_dll(void) {
    return hv_vmcall(hv_vmcall_remove_dll, 0, 0, 0);
}

static inline uint64_t hv_query_cr3_changes(void) {
    return hv_vmcall(hv_vmcall_query_cr3_changes, 0, 0, 0);
}

static inline uint64_t hv_query_anticheat_status(void) {
    return hv_vmcall(hv_vmcall_query_anticheat_status, 0, 0, 0);
}

static inline uint64_t hv_reinject_all_cr3(void) {
    return hv_vmcall(hv_vmcall_reinject_all_cr3, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

