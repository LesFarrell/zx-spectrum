; SPACE INVADERS
; 48K ZX Spectrum game, assembled at 8000h.
;
; Controls:
;   Z     move the player left
;   X     move the player right
;   SPACE fire
;   0     leave the game through the ROM restart entry
;
; The renderer works directly on the Spectrum's non-linear bitmap memory.
; Sprites are stored four bytes wide and pre-shifted into eight horizontal
; variants during startup, which keeps the drawing loops small and fast.

    ORG $8000
; ENT is not required by the integrated assembler

; ---------------------------------------------------------------------------
; Game constants
;
; Each invader/projectile record occupies 32 bytes. Only the fields below are
; currently used; the padding makes table indexing cheap and leaves room for
; additional state.

    lInv         EQU 32
    nInv         EQU 45       ; Five rows of nine invaders
    InvStep      EQU 3
    InvSp        EQU 30       ; Ticks between invader updates
    InvVStep     EQU 3
    InvLeftMar   EQU 14
    InvRightMar  EQU 244
    nProj        EQU 4        ; Alien projectile slots; slot zero is the player shot
    BLeft        EQU 15
    SLeft        EQU 15
    BRight       EQU 240
    SRight       EQU 240
    ShipVel      EQU 34       ; Delay between bonus-ship horizontal steps
    ShipTime     EQU 20000    ; Delay before trying to launch the bonus ship

; Byte offsets within an entry in InvTable or ProjTable.
; TgType bit 7 marks an explosion and bit 0 selects a zigzag projectile.
    TgAlive      EQU 0
    TgX          EQU 1
    TgY          EQU 2
    TgSpeedI     EQU 3
    TgSpeed      EQU 4
    TgType       EQU 5
    TgMissDisp   EQU 6
    TgMissInv    EQU 7
    TgMissCount  EQU 8

; Runtime actor tables live high in RAM, away from the assembled program and
; the display file.
    InvTable     EQU $E000
    ProjTable    EQU $F000

; ---------------------------------------------------------------------------
; Entry point and one-time setup

Start:
    DI
    CALL InitGame
    CALL Game
    JR Start
InitGame:
    ; Set the initial game counters and build every shifted sprite variant.
    LD A,3
    LD (nBases),A
    LD A,0
    LD (nSheets),A
    CALL InitBaseSh
    CALL InitInvSh
    CALL InitExplSh
    RET
nBases:
    NOP
nSheets:
    NOP

; Build the eight pixel-aligned versions of the player/base graphic. The first
; definition is copied verbatim; IBSa shifts each following row one bit right.
InitBaseSh:
    LD HL,BaseDef1
    LD DE,BaseDefn
    LD BC,32
    LDIR
    LD HL,BaseDefn
    LD DE,BaseDefn+32
    LD B,56
IBSa:
    ; Rotate four source bytes through carry to produce one shifted scanline.
    OR A
    LD A,(HL)
    RRA
    LD (DE),A
    INC DE
    INC HL
    LD A,(HL)
    RRA
    LD (DE),A
    INC DE
    INC HL
    LD A,(HL)
    RRA
    LD (DE),A
    INC DE
    INC HL
    LD A,(HL)
    RRA
    LD (DE),A
    INC DE
    INC HL
    DJNZ IBSa
    RET
InitInvSh:
    ; Six invader frames, each expanded from 44 bytes to 8 * 44 bytes.
    LD B,6
    LD IX,InvShapes
    LD IY,InvDef0
IIShlp:
    PUSH BC
    CALL InitInv1
    POP BC
    LD DE,44
    ADD IY,DE
    LD DE,352
    ADD IX,DE
    DJNZ IIShlp
    RET
InitExplSh:
    ; Four explosion frames plus the bonus-ship sprite.
    LD B,5
    LD IX,Expl1
    LD IY,ExplDefn1
IEShlp:
    PUSH BC
    CALL InitExp1
    POP BC
    LD DE,32
    ADD IY,DE
    LD DE,256
    ADD IX,DE
    DJNZ IEShlp
    RET
InitExp1:
    ; Copy one 8-row definition, then generate its seven shifted variants.
    PUSH IY
    POP HL
    PUSH IX
    POP DE
    LD BC,32
    LDIR
    PUSH IX
    POP DE
    LD HL,32
    ADD HL,DE
    EX DE,HL
    LD B,56
    JP IBSa
InitInv1:
    ; Invaders are 11 rows high, so each shifted frame occupies 8 * 44 bytes.
    PUSH IY
    POP HL
    PUSH IX
    POP DE
    LD BC,44
    LDIR
    PUSH IX
    POP DE
    LD HL,44
    ADD HL,DE
    EX DE,HL
    LD B,77
    JP IBSa
InitInvaders:
    ; Create a 9-by-5 formation in InvTable. D is the row and E the column.
    LD IX,InvTable
    LD DE,0
    LD C,5
II1:
    LD B,9
    PUSH DE
II2:
    PUSH BC
    CALL CreateInv
    PUSH DE
    LD DE,lInv
    ADD IX,DE
    POP DE
    INC E
    POP BC
    DJNZ II2
    POP DE
    INC D
    DEC C
    JR NZ,II1
    LD A,nInv
    LD (NoInv1),A
    LD (cInv),A
    LD A,1
    LD (InvDir),A
    LD A,0
    LD (ChangeDir),A
    RET
    CreateInv    EQU $
             ; IX = current InvTable entry
             ; E  = formation column
             ; D  = formation row
    LD (IX+TgType),D
    LD (IX+TgAlive),1
    LD A,E
    ADD A,A
    ADD A,A
    ADD A,A
    ADD A,A
    ADD A,E
    ADD A,E
    ADD A,12
    LD (IX+TgX),A
    LD A,D
    ADD A,A
    ADD A,A
    ADD A,A
    ADD A,A
    ADD A,8
    LD B,A
    LD A,(nSheets)
    ADD A,A
    ADD A,A
    ADD A,A
    ADD A,A
    ADD A,B
    LD (IX+TgY),A
    RET
InitProj:
    ; Mark the player-shot slot and all four alien-shot slots as unused.
    LD IX,ProjTable
    LD B,nProj+1
    LD DE,lInv
IPlp:
    LD (IX+TgAlive),0
    ADD IX,DE
    DJNZ IPlp
    RET
InitBase:
    ; Reset the player position and the per-wave death flag.
    XOR A
    LD (KilledF),A
    LD A,31
    LD (BaseX),A
    RET
Game:
    ; Start a wave. A cleared formation advances nSheets; a lost base consumes
    ; one of nBases before another wave is initialized.
    CALL InitInvaders
    CALL InitProj
    CALL InitBase
    CALL PlaySheet
    LD A,(KilledF)
    OR A
    JR Z,GameNS
    LD A,(nBases)
    DEC A
    LD (nBases),A
    RET Z
    JR Game
GameNS:
    LD A,(nSheets)
    INC A
    LD (nSheets),A
    JR Game
KilledF:
    NOP

; ---------------------------------------------------------------------------
; Main wave loop and screen setup

PlaySheet:
    ; Draw the static scene, place all actors, and disable the bonus ship until
    ; its countdown expires. Writing C9h over MoveShip is a one-byte RET patch.
    CALL CLS
    CALL DrawShields
    CALL ORBase
    CALL DisplayInvs
    XOR A
    LD (EndF),A
    LD A,$C9
    LD (MoveShip),A
    LD HL,ShipTime
    LD (NextShipT),HL
PlayLoop:
    ; Cooperative update loop: each subsystem performs one small step and
    ; returns. Reading keyboard row EFFEh lets key 0 return to the ROM.
    CALL MoveBase
    CALL MoveProj
    CALL MoveInvs
    CALL MoveShip
    CALL NextShip
    LD BC,61438
    IN A,(C)
    BIT 0,A
    JP Z,0
    LD A,(EndF)
    OR A
    RET NZ
    JR PlayLoop
EndF:
    NOP
CLS:
    ; Clear the 6144-byte bitmap, then paint broad attribute bands for the
    ; playfield. Port FEh sets a black border.
    LD HL,$4000
    LD DE,$4001
    LD BC,$1800
    LD (HL),0
    LDIR
    LD (HL),$06
    LD BC,32
    LDIR
    LD (HL),$07
    LD BC,$240
    LDIR
    LD (HL),$04
    LD BC,$9F
    LDIR
    LD A,0
    OUT ($FE),A
    RET
DrawShields:
    ; Draw four copies of the 28-row shield, eight character columns apart.
    LD B,4
    LD A,2
DS1:
    CALL Shield
    ADD A,8
    DJNZ DS1
    RET
Shield:
    ; A is the shield's character-column offset. ShieldYs maps each logical
    ; row to its physical Spectrum bitmap address.
    PUSH BC
    LD IX,ShieldYs
    LD HL,ShieldDefn
    LD B,28
Shield1:
    PUSH BC
    EX DE,HL
    LD C,A
    LD B,0
    LD L,(IX)
    LD H,(IX+1)
    ADD HL,BC
    EX DE,HL
    LD BC,4
    LDIR
    POP BC
    INC IX
    INC IX
    DJNZ Shield1
    POP BC
    RET
ORShip:
    ; Select the bonus ship's pre-shifted bitmap and merge it onto the screen.
    LD A,(ShipX)
    SUB 12
    AND $7
    RRCA
    RRCA
    RRCA
    LD HL,Ship1
    LD E,A
    LD D,0
    ADD HL,DE
    EX DE,HL
    LD A,(ShipX)
    SUB 12
    RRCA
    RRCA
    RRCA
    AND $1F
    LD IX,ShipYs
    JR ORIt
ANDShip:
    ; Erase the bonus ship by ANDing the screen with the inverse sprite mask.
    LD A,(ShipX)
    SUB 12
    AND $7
    RRCA
    RRCA
    RRCA
    LD HL,Ship1
    LD E,A
    LD D,0
    ADD HL,DE
    EX DE,HL
    LD A,(ShipX)
    SUB 12
    RRCA
    RRCA
    RRCA
    AND $1F
    LD IX,ShipYs
    JP ANDIt
ORBase:
    ; Draw the player's base at BaseX.
    LD A,(BaseX)
    SUB 12
    AND $7
    RRCA
    RRCA
    RRCA
    LD HL,BaseDefn
    LD E,A
    LD D,0
    ADD HL,DE
    EX DE,HL
    LD A,(BaseX)
    SUB 12
    RRCA
    RRCA
    RRCA
    AND $1F
    LD IX,BaseYs
ORIt:
    ; Common 8-row, 4-byte-wide sprite merge. DE is sprite data, IX is a table
    ; of screen row addresses, and A is the byte-column offset.
    LD B,8
DBlp:
    PUSH BC
    LD L,(IX)
    LD H,(IX+1)
    LD C,A
    LD B,0
    ADD HL,BC
    LD B,4
    LD C,A
DBlp1:
    LD A,(DE)
    OR (HL)
    LD (HL),A
    INC DE
    INC HL
    DJNZ DBlp1
    LD A,C
    POP BC
    INC IX
    INC IX
    DJNZ DBlp
    RET
ORInvRows:
    ; Invaders are 11 rows high; otherwise this is the same merge as ORIt.
    LD B,11
OIlp:
    PUSH BC
    LD L,(IX)
    LD H,(IX+1)
    LD C,A
    LD B,0
    ADD HL,BC
    LD B,4
    LD C,A
OIlp1:
    LD A,(DE)
    OR (HL)
    LD (HL),A
    INC DE
    INC HL
    DJNZ OIlp1
    LD A,C
    POP BC
    INC IX
    INC IX
    DJNZ OIlp
    RET
ANDBase:
    ; Select the player's shifted mask, then fall through to the common eraser.
    LD A,(BaseX)
    SUB 12
    AND $7
    RRCA
    RRCA
    RRCA
    LD HL,BaseDefn
    LD E,A
    LD D,0
    ADD HL,DE
    EX DE,HL
    LD A,(BaseX)
    SUB 12
    RRCA
    RRCA
    RRCA
    AND $1F
    LD IX,BaseYs
ANDIt:
    ; Common 8-row eraser: screen = screen AND NOT sprite.
    LD B,8
Dilp:
    PUSH BC
    LD L,(IX)
    LD H,(IX+1)
    LD C,A
    LD B,0
    ADD HL,BC
    LD B,4
    LD C,A
Dilp1:
    LD A,(DE)
    CPL
    AND (HL)
    LD (HL),A
    INC DE
    INC HL
    DJNZ Dilp1
    LD A,C
    POP BC
    INC IX
    INC IX
    DJNZ Dilp
    RET
ANDInvRows:
    ; Eleven-row version of the common sprite eraser.
    LD B,11
AIlp:
    PUSH BC
    LD L,(IX)
    LD H,(IX+1)
    LD C,A
    LD B,0
    ADD HL,BC
    LD B,4
    LD C,A
AIlp1:
    LD A,(DE)
    CPL
    AND (HL)
    LD (HL),A
    INC DE
    INC HL
    DJNZ AIlp1
    LD A,C
    POP BC
    INC IX
    INC IX
    DJNZ AIlp
    RET
DisplayInvs:
    ; Walk all 45 records and draw entries whose TgAlive byte is nonzero.
    LD B,nInv
    LD IX,InvTable
DisplayInvsLoop:
    LD A,(IX+TgAlive)
    OR A
    CALL NZ,ORInv
    LD DE,lInv
    ADD IX,DE
    DJNZ DisplayInvsLoop
    RET
ORInv:
    ; Draw one invader. CalcInvAdd selects its animation frame and horizontal
    ; shift; LineYs supplies the non-linear screen addresses for its Y position.
    PUSH BC
    PUSH IX
    CALL CalcInvAdd
    PUSH HL
    LD A,(IX+TgY)
    ADD A,A
    LD C,A
    LD A,0
    ADC A,0
    LD B,A
    LD HL,LineYs
    ADD HL,BC
    PUSH HL
    LD A,(IX+TgX)
    SUB 12
    RRCA
    RRCA
    RRCA
    AND $1F
    POP IX
    POP DE
    CALL ORInvRows
    POP IX
    POP BC
    RET
ANDInv:
    ; Erase one invader using the same address calculation as ORInv.
    PUSH BC
    PUSH IX
    CALL CalcInvAdd
    PUSH HL
    LD A,(IX+TgY)
    ADD A,A
    LD C,A
    LD A,0
    ADC A,0
    LD B,A
    LD HL,LineYs
    ADD HL,BC
    PUSH HL
    LD A,(IX+TgX)
    SUB 12
    RRCA
    RRCA
    RRCA
    AND $1F
    POP IX
    POP DE
    CALL ANDInvRows
    POP IX
    POP BC
    RET
CalcInvAdd:
    ; Return HL = shifted invader bitmap:
    ;   InvShapes + TgType * 352 + ((TgX - 12) & 7) * 44
    LD A,(IX+TgType)
    LD DE,352
    LD HL,InvShapes
CIA:
    OR A
    JR Z,CIA1
    ADD HL,DE
    DEC A
    JR CIA
CIA1:
    LD A,(IX+TgX)
    SUB 12
    AND $7
    LD DE,44
CIA2:
    OR A
    RET Z
    ADD HL,DE
    DEC A
    JR CIA2
BaseAlive:
    DB 1
BaseX:
    DB 10
BaseY:
    DB 191-8
BaseSpeed:
    DB 1
FireP:
    DB 0

; ---------------------------------------------------------------------------
; Player input and movement

MoveBase:
    ; Rate-limit movement, then read SPACE from keyboard row 7FFEh. FireP
    ; prevents auto-repeat while the key remains held.
    LD A,(BaseSpeed)
    DEC A
    LD (BaseSpeed),A
    JR NZ,MB4
    LD A,25
    LD (BaseSpeed),A
    CALL MB3
MB4:
    LD BC,32766
    IN A,(C)
    BIT 0,A
    JR NZ,MB1
    LD A,(FireP)
    OR A
    LD A,1
    LD (FireP),A
    RET NZ
    LD D,0-3&$FF
    LD E,191-15
    LD B,20
    LD C,0
    LD IX,BaseAlive
    CALL FireProjB
    RET
MB1:
    XOR A
    LD (FireP),A
    RET
MB3:
    ; Read Z and X from keyboard row FEFEh. The sprite is erased before BaseX
    ; changes and redrawn afterward.
    LD BC,65278
    IN A,(C)
    AND $06
    JP PE,ORBase
    BIT 1,A
    JR NZ,MBRight
    LD A,(BaseX)
    CP BLeft
    RET Z
    CALL ANDBase
    LD A,(BaseX)
    DEC A
    LD (BaseX),A
    JP ORBase
MBRight:
    LD A,(BaseX)
    CP BRight
    RET Z
    CALL ANDBase
    LD A,(BaseX)
    INC A
    LD (BaseX),A
    JP ORBase
InvDir:
    DB 0
InvSpeed:
    DB 1
NoInv1:
    DB 0
NoInv2:
    DB 0
cInv:
    DB 0
pcInv:
    DW InvTable
ChangeDir:
    DB 0

; ---------------------------------------------------------------------------
; Invader movement and alien fire

MoveInvs:
    ; Update one live invader per call. Spreading the work across frames keeps
    ; animation responsive even with the full 45-member formation.
    LD A,(InvSpeed)
    DEC A
    LD (InvSpeed),A
    RET NZ
    LD A,InvSp
    LD (InvSpeed),A
MI1:
    LD A,(cInv)
    LD HL,(pcInv)
    CP nInv
    JP Z,LastInv
    INC A
    LD (cInv),A
    LD DE,lInv
    ADD HL,DE
    LD (pcInv),HL
MI1a:
    ; Erase, move, toggle the two-frame animation bit, then redraw.
    LD A,(HL)
    OR A
    JR Z,MI1
    LD A,(NoInv1)
    INC A
    LD (NoInv1),A
    PUSH HL
    POP IX
    CALL ANDInv
    CALL DoMoveInv
    LD A,(IX+TgType)
    XOR $01
    LD (IX+TgType),A
    CALL ORInv
Rnd1:
    ; Use the Z80 refresh register as a lightweight pseudo-random source.
    ; Pick one of the nine formation columns and search upward for a live alien.
    LD A,R
    AND $F
    CP 9
    JR NC,Rnd1
    ADD A,A
    ADD A,A
    ADD A,A
    ADD A,A
    ADD A,A
    LD E,A
    LD A,0
    ADC A,0
    LD D,A
    LD HL,InvTable+1024+128
    ADD HL,DE
    LD B,5
    LD DE,0-288
FindItLp:
    ; Entries in a column are 9 * 32 = 288 bytes apart.
    LD A,(HL)
    OR A
    JR NZ,FILx
    ADD HL,DE
    DJNZ FindItLp
    RET
FILx:
    PUSH HL
    POP IX
    LD A,R
    AND $50
    RET NZ
FireIN:
    ; Launch an alien projectile below the chosen invader. D is its horizontal
    ; drift, E its initial Y, B its speed, and C its projectile type.
    LD A,(IX+TgY)
    ADD A,20
    LD E,A
    LD A,R
    AND $01
    LD C,A
    NEG
    ADD A,4
    LD D,A
    LD A,R
    AND $3F
    ADD A,20
    LD B,A
    CALL FireProj
    RET C
    LD (IY+TgMissInv),1
    RET
DoMoveInv:
    ; InvDir bit 0 selects horizontal direction; bit 7 requests a downward step
    ; after the formation reaches a margin.
    LD A,(InvDir)
    BIT 7,A
    CALL NZ,DMIDown
    BIT 0,A
    JR NZ,DMIRight
    LD A,(IX+TgX)
    SUB InvStep
    LD (IX+TgX),A
    CP InvLeftMar
    RET NC
    LD A,1
    LD (ChangeDir),A
    RET
    RET
DMIRight:
    LD A,(IX+TgX)
    ADD A,InvStep
    LD (IX+TgX),A
    CP InvRightMar
    RET C
    LD A,1
    LD (ChangeDir),A
    RET
DMIDown:
    EX AF,AF'
    LD A,(IX+TgY)
    ADD A,InvVStep
    LD (IX+TgY),A
    EX AF,AF'
    RET
LastInv:
    ; End the wave if no live invader was visited. Otherwise begin another
    ; table pass and reverse/drop the formation when ChangeDir is set.
    LD A,(NoInv1)
    LD (NoInv2),A
    OR A
    JR NZ,MI2
    LD A,1
    LD (EndF),A
    RET
MI2:
    XOR A
    LD (NoInv1),A
    INC A
    LD (cInv),A
    LD A,(InvDir)
    AND $1
    LD (InvDir),A
    LD HL,InvTable
    LD (pcInv),HL
    LD A,(ChangeDir)
    OR A
    JP Z,MI1a
    XOR A
    LD (ChangeDir),A
    LD A,(InvDir)
    OR $80
    XOR $01
    LD (InvDir),A
    JP MI1a
MoveShip:
    ; Bonus-ship update. The first byte is self-modified between NOP (active)
    ; and RET (inactive) by NextShip, MSexit, and the hit handler.
    NOP
    LD A,(ShipSpeed)
    DEC A
    LD (ShipSpeed),A
    RET NZ
    LD A,ShipVel
    LD (ShipSpeed),A
    CALL ANDShip
    LD A,(ShipDelX)
    LD B,A
    LD A,(ShipX)
    ADD A,B
    LD (ShipX),A
    LD B,A
    LD A,(TargetMargin)
    CP B
    JR Z,MSexit
    CALL ORShip
    RET
MSexit:
    LD A,$C9
    LD (MoveShip),A
    RET
ShipSpeed:
    DB 1
ShipX:
    DB SLeft
ShipDelX:
    DB 1
TargetMargin:
    DB SRight
NextShip:
    ; Periodically enable a bonus ship once the formation has been reduced.
    ; Its starting edge and travel direction are selected from R bit 0.
    LD HL,(NextShipT)
    DEC HL
    LD (NextShipT),HL
    LD A,L
    OR H
    RET NZ
    LD HL,ShipTime
    LD (NextShipT),HL
    LD A,(NoInv2)
    CP 8
    RET C
    XOR A
    LD (MoveShip),A
    LD A,R
    AND $01
    JR Z,NSRight
    LD A,SLeft
    LD (TargetMargin),A
    LD A,SRight
    LD (ShipX),A
    LD A,1
    LD (ShipSpeed),A
    LD A,0-1&$FF
    LD (ShipDelX),A
    RET
NSRight:
    LD A,SRight
    LD (TargetMargin),A
    LD A,SLeft
    LD (ShipX),A
    LD A,1
    LD (ShipSpeed),A
    LD A,1
    LD (ShipDelX),A
    RET
NextShipT:
    DW ShipTime

; ---------------------------------------------------------------------------
; Projectile and explosion updates

MoveProj:
    ; Slot zero is the player's shot; the following nProj slots belong to the
    ; invaders. Each active entry is erased, advanced, and redrawn.
    LD B,nProj+1
    LD IX,ProjTable
MPlp:
    LD A,(IX+TgAlive)
    OR A
    CALL NZ,MP1
    LD DE,lInv
    ADD IX,DE
    DJNZ MPlp
    RET
MP1:
    ; TgSpeed counts down to the next movement. TgMissCount cycles through the
    ; three zigzag masks used by guided alien shots.
    DEC (IX+TgSpeed)
    RET NZ
    LD A,(IX+TgSpeedI)
    LD (IX+TgSpeed),A
    PUSH BC
    CALL RemoveProj
    LD A,(IX+TgMissCount)
    INC A
    CP 3
    JR NZ,MP1z
    XOR A
MP1z:
    LD (IX+TgMissCount),A
    BIT 7,(IX+TgType)
    JR NZ,MP1a
    LD A,(IX+TgY)
    ADD A,(IX+TgMissDisp)
    LD (IX+TgY),A
    CP 4
    JR C,MP2
    CP 191-4
    JR NC,MP2
MP1a:
    CALL PlaceProj
    POP BC
    RET
MP2:
    CALL Explode
    POP BC
    RET
Explode:
    ; Replace a projectile with a four-tick explosion actor.
    CALL RemoveProj
    LD (IX+TgAlive),4
    LD (IX+TgType),$80
    LD (IX+TgSpeed),50
    LD (IX+TgSpeedI),50
    RET
ANDBar:
    ; Erase a straight one-pixel-wide projectile from eight bitmap rows.
    LD A,(IX+TgX)
    AND $07
    LD HL,BitTable
    LD E,A
    LD D,0
    ADD HL,DE
    LD A,(HL)
    CPL
    LD C,A
    LD A,(IX+TgY)
    LD L,A
    LD H,0
    ADD HL,HL
    LD DE,LineYs
    ADD HL,DE
    PUSH HL
    POP IY
    LD A,(IX+TgX)
    RRCA
    RRCA
    RRCA
    AND $1F
    LD E,A
    LD D,0
    LD B,8
RPlp:
    LD L,(IY)
    LD H,(IY+1)
    ADD HL,DE
    EX AF,AF'
    LD A,(HL)
    AND C
    LD (HL),A
    EX AF,AF'
    INC IY
    INC IY
    DJNZ RPlp
    RET
ORBar:
    ; Draw a straight projectile. Any occupied destination pixel is a collision,
    ; so the shot becomes an explosion and TestAndKill identifies the target.
    XOR A
    LD (ProjHit),A
    LD A,(IX+TgX)
    AND $07
    LD HL,BitTable
    LD E,A
    LD D,0
    ADD HL,DE
    LD A,(HL)
    LD C,A
    LD A,(IX+TgY)
    LD L,A
    LD H,0
    ADD HL,HL
    LD DE,LineYs
    ADD HL,DE
    PUSH HL
    POP IY
    LD A,(IX+TgX)
    RRCA
    RRCA
    RRCA
    AND $1F
    LD E,A
    LD D,0
    LD B,8
PPlp:
    LD L,(IY)
    LD H,(IY+1)
    ADD HL,DE
    EX AF,AF'
    LD A,(HL)
    AND C
    JR NZ,PPlp1
    LD A,(HL)
    OR C
    LD (HL),A
    EX AF,AF'
    INC IY
    INC IY
    DJNZ PPlp
    RET
PPlp1:
    CALL Explode
    PUSH IX
    CALL TestAndKill
    POP IX
    RET
ProjHit:
    NOP
ZigCalc:
    ; Select one of three 128-byte zigzag phases and calculate the screen byte
    ; and row-address pointer shared by the zigzag draw/erase routines.
    LD A,(IX+TgX)
    SUB 4
    AND $07
    ADD A,A
    ADD A,A
    ADD A,A
    ADD A,A
    LD HL,ZigTable
    LD E,A
    LD D,0
    LD A,(IX+TgMissCount)
    OR A
    JR Z,ZC1
    LD HL,ZigTable+128
    DEC A
    JR Z,ZC1
    LD HL,ZigTable+256
ZC1:
    ADD HL,DE
    PUSH HL
    LD A,(IX+TgY)
    LD L,A
    LD H,0
    ADD HL,HL
    LD DE,LineYs
    ADD HL,DE
    PUSH HL
    POP IY
    LD A,(IX+TgX)
    SUB 4
    RRCA
    RRCA
    RRCA
    AND $1F
    LD E,A
    LD D,0
    LD B,8
    EXX
    POP HL
    EXX
    RET
ANDZig:
    ; Erase an eight-row, two-byte-wide zigzag projectile.
    CALL ZigCalc
ZAlp:
    LD L,(IY)
    LD H,(IY+1)
    ADD HL,DE
    EX AF,AF'
    EXX
    LD A,(HL)
    INC HL
    EXX
    CPL
    AND (HL)
    LD (HL),A
    INC HL
    EXX
    LD A,(HL)
    INC HL
    EXX
    CPL
    AND (HL)
    LD (HL),A
    EX AF,AF'
    INC IY
    INC IY
    DJNZ ZAlp
    RET
ORZig:
    ; Draw the zigzag mask, branching to the normal collision handler when
    ; either destination byte is already occupied.
    CALL ZigCalc
ZOlp:
    LD L,(IY)
    LD H,(IY+1)
    ADD HL,DE
    EX AF,AF'
    EXX
    LD A,(HL)
    EXX
    AND (HL)
    JP NZ,PPlp1
    EXX
    LD A,(HL)
    INC HL
    EXX
    OR (HL)
    LD (HL),A
    INC HL
    EXX
    LD A,(HL)
    EXX
    AND (HL)
    JP NZ,PPlp1
    EXX
    LD A,(HL)
    INC HL
    EXX
    OR (HL)
    LD (HL),A
    EX AF,AF'
    INC IY
    INC IY
    DJNZ ZOlp
    RET
ZigTable:
    ; Three animation phases. Each phase contains eight horizontal alignments;
    ; each alignment is an 8-row by 2-byte mask (8 * 8 * 2 = 128 bytes).
    DB $08,$00
    DB $10,$00
    DB $08,$00
    DB $04,$00
    DB $08,$00
    DB $10,$00
    DB $08,$00
    DB $04,$00
;0
    DB $04,$00
    DB $08,$00
    DB $04,$00
    DB $02,$00
    DB $04,$00
    DB $08,$00
    DB $04,$00
    DB $02,$00
;1
    DB $02,$00
    DB $04,$00
    DB $02,$00
    DB $01,$00
    DB $02,$00
    DB $04,$00
    DB $02,$00
    DB $01,$00
;2
    DB $01,$00
    DB $02,$00
    DB $01,$00
    DB $00,$80
    DB $01,$00
    DB $02,$00
    DB $01,$00
    DB $00,$80
;3
    DB $00,$80
    DB $01,$00
    DB $00,$80
    DB $00,$40
    DB $00,$80
    DB $01,$00
    DB $00,$80
    DB $00,$40
;4
    DB $00,$40
    DB $00,$80
    DB $00,$40
    DB $00,$20
    DB $00,$40
    DB $00,$80
    DB $00,$40
    DB $00,$20
;5
    DB $00,$20
    DB $00,$40
    DB $00,$20
    DB $00,$10
    DB $00,$20
    DB $00,$40
    DB $00,$20
    DB $00,$10
;6
    DB $00,$10
    DB $00,$20
    DB $00,$10
    DB $00,$08
    DB $00,$10
    DB $00,$20
    DB $00,$10
    DB $00,$08
;7
    DB $10,$00
    DB $08,$00
    DB $04,$00
    DB $08,$00
    DB $10,$00
    DB $08,$00
    DB $04,$00
    DB $08,$00
;0
    DB $08,$00
    DB $04,$00
    DB $02,$00
    DB $04,$00
    DB $08,$00
    DB $04,$00
    DB $02,$00
    DB $04,$00
;1
    DB $04,$00
    DB $02,$00
    DB $01,$00
    DB $02,$00
    DB $04,$00
    DB $02,$00
    DB $01,$00
    DB $02,$00
;2
    DB $02,$00
    DB $01,$00
    DB $00,$80
    DB $01,$00
    DB $02,$00
    DB $01,$00
    DB $00,$80
    DB $01,$00
;3
    DB $01,$00
    DB $00,$80
    DB $00,$40
    DB $00,$80
    DB $01,$00
    DB $00,$80
    DB $00,$40
    DB $00,$80
;4
    DB $00,$80
    DB $00,$40
    DB $00,$20
    DB $00,$40
    DB $00,$80
    DB $00,$40
    DB $00,$20
    DB $00,$40
;5
    DB $00,$40
    DB $00,$20
    DB $00,$10
    DB $00,$20
    DB $00,$40
    DB $00,$20
    DB $00,$10
    DB $00,$20
;6
    DB $00,$20
    DB $00,$10
    DB $00,$08
    DB $00,$10
    DB $00,$20
    DB $00,$10
    DB $00,$08
    DB $00,$10
;7
    DB $08,$00
    DB $04,$00
    DB $08,$00
    DB $10,$00
    DB $08,$00
    DB $04,$00
    DB $08,$00
    DB $10,$00
;0
    DB $04,$00
    DB $02,$00
    DB $04,$00
    DB $08,$00
    DB $04,$00
    DB $02,$00
    DB $04,$00
    DB $08,$00
;1
    DB $02,$00
    DB $01,$00
    DB $02,$00
    DB $04,$00
    DB $02,$00
    DB $01,$00
    DB $02,$00
    DB $04,$00
;2
    DB $01,$00
    DB $00,$80
    DB $01,$00
    DB $02,$00
    DB $01,$00
    DB $00,$80
    DB $01,$00
    DB $02,$00
;3
    DB $00,$80
    DB $00,$40
    DB $00,$80
    DB $01,$00
    DB $00,$80
    DB $00,$40
    DB $00,$80
    DB $01,$00
;4
    DB $00,$40
    DB $00,$20
    DB $00,$40
    DB $00,$80
    DB $00,$40
    DB $00,$20
    DB $00,$40
    DB $00,$80
;5
    DB $00,$20
    DB $00,$10
    DB $00,$20
    DB $00,$40
    DB $00,$20
    DB $00,$10
    DB $00,$20
    DB $00,$40
;6
    DB $00,$10
    DB $00,$08
    DB $00,$10
    DB $00,$20
    DB $00,$10
    DB $00,$08
    DB $00,$10
    DB $00,$20
;7

RemoveProj:
    ; Dispatch erasing by TgType: bit 7 = explosion, bit 0 = zigzag, otherwise
    ; use the straight vertical bar.
    LD A,(IX+TgType)
    BIT 7,A
    JR NZ,RemExpl
    BIT 0,(IX+TgType)
    JP NZ,ANDZig
    JP ANDBar
PlaceProj:
    ; Matching draw dispatcher for explosions, zigzags, and straight shots.
    LD A,(IX+TgType)
    BIT 7,A
    JR NZ,PlaceExpl
    BIT 0,(IX+TgType)
    JP NZ,ORZig
    JP ORBar
RemExpl:
    ; TgType low bits select one of four explosion frames.
    AND $3
    LD HL,Expl1
    OR A
    JR Z,RE1
    LD HL,Expl2
    DEC A
    JR Z,RE1
    LD HL,Expl3
    DEC A
    JR Z,RE1
    LD HL,Expl4
RE1:
    PUSH IX
    CALL CalcExplAdd
    CALL ANDIt
    POP IX
    RET
PlaceExpl:
    ; Count down TgAlive and choose a pseudo-random explosion frame.
    DEC (IX+TgAlive)
    RET Z
    LD A,R
    AND $3
    LD (IX+TgType),A
    SET  7,(IX+TgType)
    LD HL,Expl1
    OR A
    JR Z,PE1
    LD HL,Expl2
    DEC A
    JR Z,PE1
    LD HL,Expl3
    DEC A
    JR Z,PE1
    LD HL,Expl4
PE1:
    PUSH IX
    CALL CalcExplAdd
    CALL ORIt
    POP IX
    RET
KillExpl:
    LD (IX+TgAlive),0
    RET
CalcExplAdd:
    ; Convert an explosion's X/Y position to a shifted sprite pointer in DE,
    ; an X byte offset in A, and a screen-row table pointer in IX.
    LD A,(IX+TgX)
    SUB 8
    AND $7
    RRCA
    RRCA
    RRCA
    LD E,A
    LD D,0
    ADD HL,DE
    EX DE,HL
    LD BC,LineYs
    LD L,(IX+TgY)
    LD H,0
    ADD HL,HL
    ADD HL,BC
    PUSH HL
    LD A,(IX+TgX)
    SUB 8
    RRCA
    RRCA
    RRCA
    AND $1F
    POP IX
    RET

TestAndKill:
    ; Resolve a projectile collision. Alien missiles test the player's base;
    ; the player shot first tests the bonus ship, then scans all invaders.
    LD A,(IX+TgMissInv)
    OR A
    JR NZ,HBase
    LD L,(IX+TgX)
    LD A,(IX+TgY)
    CP 8
    JP C,HShip
    LD H,A
    LD B,nInv
    LD IY,InvTable
TAKlp:
    ; A live invader is hit when the projectile lies within its 9-by-16 pixel
    ; collision box.
    LD A,(IY+TgAlive)
    OR A
    JR Z,TAKlpx
    LD A,(IY+TgX)
    SUB L
    JP P,TAK1
    NEG
TAK1:
    CP 9
    JR NC,TAKlpx
    LD A,H
    SUB (IY+TgY)
    JR C,TAKlpx
    CP 16
    JR NC,TAKlpx
             ; Found an invader: erase it and clear its live flag.
    PUSH IY
    POP IX
    CALL ANDInv
    LD (IY+TgAlive),0
    RET
TAKlpx:
    LD DE,lInv
    ADD IY,DE
    DJNZ TAKlp
    RET
HBase:
    ; Alien projectile reached the player's Y band. A horizontal overlap
    ; restarts the game from its entry point.
    LD A,(IX+TgY)
    CP 191-13
    RET C
    LD A,(BaseX)
    SUB (IX+TgX)
    JP P,HB1
    NEG
HB1:
    CP 14
    RET NC
    JP $8000
HShip:
    ; The bonus ship can only be hit while MoveShip is active (first byte NOP).
    LD A,(MoveShip)
    OR A
    RET NZ
    LD A,(ShipX)
    SUB (IX+TgX)
    JP P,HSp1
    NEG
HSp1:
    CP 10
    RET NC
             ; Hit: erase and disable the bonus ship.
    CALL ANDShip
    LD A,$C9
    LD (MoveShip),A
    RET
FireProjB:
    ; Allocate the dedicated player-shot slot, if it is free.
    LD IY,ProjTable
    LD A,(IY+TgAlive)
    OR A
    JR Z,FPB
    RET
FireProj:
    ; Allocate an alien-shot slot. Carry is returned set when all slots are busy.
    PUSH DE ; D = Disp E = Y
    PUSH BC ; B = Speed C = Type
    CALL FPS
    POP BC
    POP DE
    RET C
FPB:
    ; Initialize the slot selected in IY from the caller's B/C/D/E parameters.
    LD (IY+TgSpeed),1
    LD (IY+TgMissCount),0
    LD (IY+TgSpeedI),B
    LD (IY+TgType),C
    LD A,(IX+TgX)
    LD (IY+TgX),A
    LD (IY+TgY),E
    LD (IY+TgAlive),1
    LD (IY+TgMissDisp),D
    LD (IY+TgMissInv),0
    OR A
    RET
FPS:
    ; Search only the nProj alien slots; slot zero is reserved for the player.
    LD IY,ProjTable+lInv
    LD B,nProj
    LD DE,lInv
FPS1:
    LD A,(IY+TgAlive)
    OR A
    RET Z
    ADD IY,DE
    DJNZ FPS1
    SCF
    RET
BitTable:
    ; Pixel masks for X coordinates within a screen byte.
    DB $80,$40,$20,$10,$08,$04,$02,$01
    DW 0,0,0,0,0,0,0,0,0,0,0,0
    DW 0,0,0,0,0,0,0,0,0,0,0,0
    DW 0,0,0,0,0,0,0,0,0,0,0,0
LineYs:
    ; Spectrum bitmap memory is interleaved rather than linear. This table maps
    ; each pixel row to the address of its first screen byte. ShipYs, ShieldYs,
    ; and BaseYs are aliases/pointers into the appropriate vertical regions.
    DW $4000,$4100,$4200,$4300
    ShipYs       EQU LineYs
    DW $4400,$4500,$4600,$4700
    DW $4020,$4120,$4220,$4320
    DW $4420,$4520,$4620,$4720
    DW $4040,$4140,$4240,$4340
    DW $4440,$4540,$4640,$4740
    DW $4060,$4160,$4260,$4360
    DW $4460,$4560,$4660,$4760
    DW $4080,$4180,$4280,$4380
    DW $4480,$4580,$4680,$4780
    DW $40A0,$41A0,$42A0,$43A0
    DW $44A0,$45A0,$46A0,$47A0
    DW $40C0,$41C0,$42C0,$43C0
    DW $44C0,$45C0,$46C0,$47C0
    DW $40E0,$41E0,$42E0,$43E0
    DW $44E0,$45E0,$46E0,$47E0
    DW $4800,$4900,$4A00,$4B00
    DW $4C00,$4D00,$4E00,$4F00
    DW $4820,$4920,$4A20,$4B20
    DW $4C20,$4D20,$4E20,$4F20
    DW $4840,$4940,$4A40,$4B40
    DW $4C40,$4D40,$4E40,$4F40
    DW $4860,$4960,$4A60,$4B60
    DW $4C60,$4D60,$4E60,$4F60
    DW $4880,$4980,$4A80,$4B80
    DW $4C80,$4D80,$4E80,$4F80
    DW $48A0,$49A0,$4AA0,$4BA0
    DW $4CA0,$4DA0,$4EA0,$4FA0
    DW $48C0,$49C0,$4AC0,$4BC0
    DW $4CC0,$4DC0,$4EC0,$4FC0
    DW $48E0,$49E0,$4AE0,$4BE0
    DW $4CE0,$4DE0,$4EE0,$4FE0
    DW $5000,$5100,$5200,$5300
    DW $5400,$5500,$5600,$5700
    DW $5020,$5120,$5220,$5320
    DW $5420,$5520,$5620,$5720
    DW $5040,$5140,$5240,$5340
    DW $5440,$5540,$5640,$5740
ShieldYs:
    DW $5060,$5160,$5260,$5360
    DW $5460,$5560,$5660,$5760
    DW $5080,$5180,$5280,$5380
    DW $5480,$5580,$5680,$5780
    DW $50A0,$51A0,$52A0,$53A0
    DW $54A0,$55A0,$56A0,$57A0
    DW $50C0,$51C0,$52C0,$53C0
    DW $54C0,$55C0,$56C0,$57C0
BaseYs:
    DW $50E0,$51E0,$52E0,$53E0
    DW $54E0,$55E0,$56E0,$57E0
    ; Padding keeps later sprite-address walks inside a harmless zero table.
    DW 0,0,0,0,0,0,0,0,0,0,0,0
    DW 0,0,0,0,0,0,0,0,0,0,0,0
    DW 0,0,0,0,0,0,0,0,0,0,0,0
    DW 0,0,0,0,0,0,0,0,0,0,0,0
ShieldDefn:
    ; One unshifted shield: 28 rows by 4 bytes. Shield copies are written
    ; directly to the display and are destructible because shots alter pixels.
    DB $00,$3F,$FC,$00
    DB $01,$FF,$FF,$80
    DB $03,$FF,$FF,$C0
    DB $07,$FF,$FF,$E0
    DB $0F,$FF,$FF,$F0
    DB $1F,$FF,$FF,$F8
    DB $3F,$FF,$FF,$FC
    DB $7F,$FF,$FF,$FE
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$FF,$FF,$FF
    DB $FF,$E0,$07,$FF
    DB $FF,$80,$01,$FF
    DB $FF,$00,$00,$FF
    DB $FE,$00,$00,$7F
    DB $FC,$00,$00,$3F
    DB $F8,$00,$00,$1F
    DB $F0,$00,$00,$0F
BaseDef1:
    ; Unshifted 8-row player base. InitBaseSh expands this to eight alignments.
    DB $00,$08,$00,$00
    DB $00,$08,$00,$00
    DB $00,$08,$00,$00
    DB $00,$7F,$00,$00
    DB $0F,$FF,$F8,$00
    DB $1F,$FF,$FC,$00
    DB $1F,$FF,$FC,$00
    DB $1F,$FF,$FC,$00
InvDef0:
    ; Six unshifted invader frames, 11 rows by 4 bytes. Adjacent pairs form the
    ; two animation poses for each of the three invader row styles.
    DB $00,$08,$00,$00
    DB $00,$1C,$00,$00
    DB $00,$3E,$00,$00
    DB $00,$7F,$00,$00
    DB $00,$C9,$80,$00
    DB $00,$C9,$80,$00
    DB $00,$FF,$80,$00
    DB $00,$FF,$80,$00
    DB $00,$2A,$00,$00
    DB $00,$41,$00,$00
    DB $00,$22,$00,$00
;Inv1
    DB $00,$08,$00,$00
    DB $00,$1C,$00,$00
    DB $00,$3E,$00,$00
    DB $00,$7F,$00,$00
    DB $00,$C9,$80,$00
    DB $00,$C9,$80,$00
    DB $00,$FF,$80,$00
    DB $00,$FF,$80,$00
    DB $00,$14,$00,$00
    DB $00,$2A,$00,$00
    DB $00,$55,$00,$00
;Inv2
    DB $00,$63,$00,$00
    DB $00,$14,$00,$00
    DB $00,$3E,$00,$00
    DB $00,$7F,$00,$00
    DB $00,$C9,$80,$00
    DB $01,$C9,$C0,$00
    DB $02,$FF,$A0,$00
    DB $02,$FF,$A0,$00
    DB $02,$E3,$A0,$00
    DB $00,$80,$80,$00
    DB $00,$63,$00,$00
;Inv3
    DB $00,$63,$00,$00
    DB $00,$14,$00,$00
    DB $02,$3E,$20,$00
    DB $02,$7F,$20,$00
    DB $02,$C9,$A0,$00
    DB $01,$C9,$C0,$00
    DB $00,$FF,$80,$00
    DB $00,$FF,$80,$00
    DB $00,$E3,$80,$00
    DB $00,$80,$80,$00
    DB $01,$00,$C0,$00
;Inv4
    DB $00,$3E,$00,$00
    DB $01,$FF,$C0,$00
    DB $03,$FF,$E0,$00
    DB $03,$08,$60,$00
    DB $03,$08,$60,$00
    DB $03,$FF,$E0,$00
    DB $03,$F7,$E0,$00
    DB $03,$E3,$E0,$00
    DB $00,$DD,$80,$00
    DB $01,$80,$C0,$00
    DB $00,$E3,$80,$00
;Inv5
    DB $00,$3E,$00,$00
    DB $01,$FF,$C0,$00
    DB $03,$FF,$E0,$00
    DB $03,$08,$60,$00
    DB $03,$08,$60,$00
    DB $03,$FF,$E0,$00
    DB $03,$F7,$E0,$00
    DB $03,$E3,$E0,$00
    DB $00,$DD,$80,$00
    DB $01,$80,$C0,$00
    DB $03,$00,$60,$00
ExplDefn1:
    ; Four 8-row explosion frames stored consecutively.
    DB $00,$80,$00,$00
    DB $01,$40,$00,$00
    DB $02,$C0,$00,$00
    DB $05,$E0,$00,$00
    DB $03,$A8,$00,$00
    DB $02,$40,$00,$00
    DB $01,$C0,$00,$00
    DB $00,$80,$00,$00
    DB $00,$80,$00,$00 ;expl 2
    DB $02,$A0,$00,$00
    DB $04,$88,$00,$00
    DB $05,$96,$00,$00
    DB $09,$DC,$00,$00
    DB $04,$A0,$00,$00
    DB $03,$B0,$00,$00
    DB $00,$80,$00,$00
    DB $00,$80,$00,$00 ;expl 3
    DB $01,$D8,$00,$00
    DB $05,$74,$00,$00
    DB $0A,$78,$00,$00
    DB $03,$E4,$00,$00
    DB $09,$58,$00,$00
    DB $05,$E0,$00,$00
    DB $00,$80,$00,$00
    DB $00,$80,$00,$00 ;expl 4
    DB $02,$40,$00,$00
    DB $05,$10,$00,$00
    DB $02,$20,$00,$00
    DB $03,$10,$00,$00
    DB $01,$50,$00,$00
    DB $01,$20,$00,$00
    DB $00,$80,$00,$00
ShipDefn:
    ; Unshifted 10-row bonus-ship graphic.
    DB $00,$00,$00,$00
    DB $00,$00,$00,$00
    DB $00,$08,$00,$00
    DB $00,$FF,$80,$00
    DB $1F,$FF,$FC,$00
    DB $6A,$AA,$AB,$00
    DB $1F,$FF,$FC,$00
    DB $00,$FF,$80,$00
    DB $00,$08,$00,$00
    DB $00,$00,$00,$00

BaseDefn:
    ; Runtime-generated, horizontally shifted sprite data. These buffers are
    ; filled during InitGame and are deliberately part of the assembled image.
    DS 256
InvShapes:
    DS 352 * 6
Expl1:
    DS 256
Expl2:
    DS 256
Expl3:
    DS 256
Expl4:
    DS 256
Ship1:
    DS 256
    ; Address immediately after the program and generated-data buffers.
    ENDY EQU $
