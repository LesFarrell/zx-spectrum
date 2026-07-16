; Demonstrates EQU constants in the built-in assembler.

START   EQU 8000h
SCREEN  EQU 4000h
ATTRS   EQU 5800h
INK_OK  EQU 4Fh
LETTER  EQU 69

ORG START

start:
	LD A,LETTER
	LD (SCREEN),A
	LD A,INK_OK
	LD (ATTRS),A
	RET
