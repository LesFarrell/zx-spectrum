ORG 8000h

start:
    LD A,1
    INCLUDE "include-shared.asm"

print_loop:
    LD A,(HL)
    OR A
    RET Z
    RST 10h
    INC HL
    JR print_loop
