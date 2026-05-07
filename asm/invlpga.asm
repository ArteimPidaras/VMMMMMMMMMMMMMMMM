.CODE

; void __invlpga(UINT64 VirtualAddress, UINT32 Asid)
; Invalidate TLB entry for a specific virtual address and ASID
__invlpga PROC
    mov rax, rcx        ; VirtualAddress in RCX
    mov ecx, edx        ; ASID in EDX
    db 0Fh, 01h, 0DFh   ; INVLPGA opcode (not all assemblers support it)
    ret
__invlpga ENDP

END
