set_border:
    OUT ($FE),A
    LD HL,message
    RET

message:
    DB 22,0,0,"MID INCLUDE OK",13,0
