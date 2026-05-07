option casemap:none

_TEXT SEGMENT ALIGN(16)

; PART 2.8: INVLPGA implementation for TLB invalidation
; void __invlpga(void* VirtualAddress, UINT32 Asid)
PUBLIC __invlpga
__invlpga PROC
    ; rcx = VirtualAddress
    ; rdx = Asid
    mov     rax, rcx
    mov     ecx, edx
    db      0Fh, 01h, 0DFh  ; invlpga rax, ecx
    ret
__invlpga ENDP

_TEXT ENDS
END
