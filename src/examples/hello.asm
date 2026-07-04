ORG 8000h

start:
    LD A,2
    CALL 1601h      ; CHAN-OPEN, select upper screen
    LD HL,text

print_loop:
    LD A,(HL)
    OR A
    RET Z
    RST 10h
    INC HL
    JR print_loop

text:
    DB 22,0,0,"HELLO WORLD",13,0