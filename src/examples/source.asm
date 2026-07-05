ORG 8000h
ld a,255
loop: dec a
out(254),a
jp nz, loop
ret
