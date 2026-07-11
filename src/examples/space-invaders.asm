; SPACE INVADERS demo for the ZX Spectrum 48K
; Built for the emulator's integrated mini assembler.
;
; Build:    Ctrl+F5 assembles the source at 8000h and runs from 8000h.
; Controls: 5 moves left, 8 moves right, and 0 fires.
;
; Gameplay: destroy the animated 3x6 formation before it reaches the player.
; Four destructible shield bases stop one laser or alien bomb per occupied cell.
; Surviving aliens launch aimed bombs from rotating formation columns.
;
; Video: eleven custom 8x8 UDGs provide two animation frames per alien type,
; the player, projectiles, explosions, and bases. Spectrum attributes colour
; individual objects while direct bitmap writes keep drawing independent of ROM.
;
; Audio: the 48K beeper supplies fire, explosion, hit, formation-step, and
; bomb-drop effects.

; UDG character assignments. Alternate alien frames are seven codes above the
; corresponding primary frame, which lets draw_swarm select them with ADD A,7.

	INVADER_TOP    EQU 144
	INVADER_MID    EQU 145
	INVADER_LOW    EQU 146
	PLAYER_CHAR    EQU 147
	LASER_CHAR     EQU 148
	BOMB_CHAR      EQU 149
	EXPLODE_CHAR   EQU 150
	INVADER_TOP_ALT EQU 151
	INVADER_MID_ALT EQU 152
	INVADER_LOW_ALT EQU 153
	BASE_CHAR       EQU 154
	SPACE_CHAR     EQU 32

	ORG 8000h

start:
	; Take control from the ROM, move the stack safely above the program, and
	; force a black border before initializing all mutable game state.
	DI
	LD SP,0FF00h
	XOR A
	OUT (0FEh),A
	CALL reset_game

main_loop:
	; One iteration updates every actor. game_state remains zero while playing,
	; becomes one after defeat, and becomes two after the final alien is removed.
	CALL frame_delay
	CALL handle_player
	CALL update_laser
	CALL update_bomb
	CALL update_swarm
	CALL update_status
	LD A,(remaining)
	OR A
	JR NZ,check_game_state
	LD A,2
	LD (game_state),A

check_game_state:
	LD A,(game_state)
	OR A
	JP Z,main_loop
	CP 2
	JP Z,show_win
	JP show_game_over
; ---------------------------------------------------------------------------
; Game setup

reset_game:
	; Draw the fixed UI first, then restore player, projectile, formation, and
	; shield state. Arrays are filled at runtime so replay does not reload code.
	CALL clear_screen
	LD D,0
	LD E,0
	LD HL,header_text
	CALL print_text
	LD D,23
	LD E,2
	LD HL,help_text
	CALL print_text
	LD A,15
	LD (player_x),A
	LD A,3
	LD (lives),A
	XOR A
	LD (score),A
	LD (game_state),A
	LD (laser_active),A
	LD (bomb_active),A
	LD (bomb_column),A
	LD (player_delay),A
	LD (animation_frame),A
	LD (base_hit),A
	LD A,2
	LD (swarm_x),A
	LD A,4
	LD (swarm_y),A
	LD A,1
	LD (swarm_dir),A
	LD A,10
	LD (move_timer),A
	LD A,35
	LD (bomb_timer),A
	LD A,18
	LD (remaining),A
	LD HL,invaders
	LD B,18
	LD A,1

reset_invaders:
	LD (HL),A
	INC HL
	DJNZ reset_invaders
	LD HL,bases
	LD B,12

reset_bases:
	LD (HL),A
	INC HL
	DJNZ reset_bases
	XOR A
	LD (swarm_paint),A
	CALL draw_swarm
	CALL draw_bases
	CALL draw_player
	CALL update_status
	RET

; ---------------------------------------------------------------------------
; Player and keyboard

handle_player:
	; Erase the old player cell before reading the keyboard. player_delay limits
	; horizontal movement while fire remains independently responsive.
	LD A,(player_x)
	LD E,A
	LD D,21
	LD A,SPACE_CHAR
	CALL print_at
	LD A,(player_delay)
	OR A
	JR Z,player_keys
	DEC A
	LD (player_delay),A
	JR player_fire

player_keys:
	CALL key_left
	JR NZ,try_right
	LD A,(player_x)
	CP 1
	JR Z,player_moved
	DEC A
	LD (player_x),A
	JR set_player_delay

try_right:
	CALL key_right
	JR NZ,player_moved
	LD A,(player_x)
	CP 30
	JR Z,player_moved
	INC A
	LD (player_x),A

set_player_delay:
	LD A,2
	LD (player_delay),A

player_moved:
player_fire:
	; Only one player laser may exist. A new shot begins one row above the player
	; and advances every main-loop iteration.
	CALL key_fire
	JR NZ,draw_player
	LD A,(laser_active)
	OR A
	JR NZ,draw_player
	LD A,1
	LD (laser_active),A
	LD A,(player_x)
	LD (laser_x),A
	LD A,20
	LD (laser_y),A
	LD A,1
	LD (laser_delay),A
	CALL fire_sound

draw_player:
	LD A,(player_x)
	LD E,A
	LD D,21
	LD A,PLAYER_CHAR
	CALL print_at
	RET

; Keyboard matrix reads. Spectrum keys are active-low: a zero result means the
; requested key is held. IN A,(C) is emitted as ED 78 because the mini assembler
; intentionally supports only the immediate-port spelling IN A,(n).
key_left:
	LD BC,0F7FEh
	DB 0EDh,078h
	AND 10h
	RET

key_right:
	LD BC,0EFFEh
	DB 0EDh,078h
	AND 04h
	RET

key_fire:
	LD BC,0EFFEh
	DB 0EDh,078h
	AND 01h
	RET

; ---------------------------------------------------------------------------
; Player laser

update_laser:
	; Erase, delay, move upward, test shields and aliens, then repaint if the shot
	; survived. Reaching row zero simply retires the projectile.
	LD A,(laser_active)
	OR A
	RET Z
	LD A,(laser_x)
	LD E,A
	LD A,(laser_y)
	LD D,A
	LD A,SPACE_CHAR
	CALL print_at
	LD A,(laser_delay)
	DEC A
	LD (laser_delay),A
	JR Z,move_laser
	CALL draw_laser
	RET

move_laser:
	LD A,1
	LD (laser_delay),A
	LD A,(laser_y)
	DEC A
	LD (laser_y),A
	CP 1
	JR NC,laser_collision
	XOR A
	LD (laser_active),A
	RET

laser_collision:
	LD A,(laser_y)
	CP 18
	JR NZ,laser_alien_collision
	LD A,(laser_x)
	CALL damage_base
	LD A,(base_hit)
	OR A
	JR Z,laser_alien_collision
	XOR A
	LD (laser_active),A
	RET

laser_alien_collision:
	CALL collide_laser
	LD A,(laser_active)
	OR A
	RET Z

draw_laser:
	LD A,(laser_x)
	LD E,A
	LD A,(laser_y)
	LD D,A
	LD A,LASER_CHAR
	CALL print_at
	RET

collide_laser:
	; HL walks the 18-byte alien alive/dead array in the same 3x6 order used by
	; draw_swarm. D/E hold each alien's current screen row and column.
	LD HL,invaders
	LD B,3
	LD A,(swarm_y)
	LD D,A

collision_row:
	LD C,6
	LD A,(swarm_x)
	LD E,A

collision_column:
	; Dead entries are skipped. A live entry is hit only when both coordinates
	; match the laser; the hit clears state and updates score and remaining count.
	LD A,(HL)
	OR A
	JR Z,collision_next
	LD A,(laser_y)
	CP D
	JR NZ,collision_next
	LD A,(laser_x)
	CP E
	JR NZ,collision_next
	XOR A
	LD (HL),A
	LD (laser_active),A
	LD A,(remaining)
	DEC A
	LD (remaining),A
	LD A,(score)
	INC A
	LD (score),A
	LD A,EXPLODE_CHAR
	CALL print_at
	CALL explosion_sound
	LD A,SPACE_CHAR
	CALL print_at
	RET

collision_next:
	INC HL
	LD A,E
	ADD A,3
	LD E,A
	DEC C
	JR NZ,collision_column
	INC D
	INC D
	DJNZ collision_row
	RET

; ---------------------------------------------------------------------------
; Alien bomb

update_bomb:
	; Alien bombs move more slowly than the laser. They can destroy a shield cell,
	; hit the player on row 21, or disappear after passing the player.
	LD A,(bomb_active)
	OR A
	JR Z,bomb_waiting
	LD A,(bomb_x)
	LD E,A
	LD A,(bomb_y)
	LD D,A
	LD A,SPACE_CHAR
	CALL print_at
	LD A,(bomb_delay)
	DEC A
	LD (bomb_delay),A
	JR Z,move_bomb
	CALL draw_bomb
	RET

move_bomb:
	LD A,3
	LD (bomb_delay),A
	LD A,(bomb_y)
	INC A
	LD (bomb_y),A
	CP 18
	JR NZ,bomb_player_check
	LD A,(bomb_x)
	CALL damage_base
	LD A,(base_hit)
	OR A
	JR Z,bomb_player_check
	XOR A
	LD (bomb_active),A
	RET

bomb_player_check:
	LD A,(bomb_y)
	CP 21
	JP C,draw_bomb
	JR NZ,remove_bomb
	LD A,(bomb_x)
	LD B,A
	LD A,(player_x)
	CP B
	JR NZ,remove_bomb
	CALL player_hit
	RET

remove_bomb:
	XOR A
	LD (bomb_active),A
	RET

bomb_waiting:
	; A countdown spaces bomb launches. Only one alien bomb is active at a time.
	LD A,(bomb_timer)
	DEC A
	LD (bomb_timer),A
	RET NZ
	LD A,35
	LD (bomb_timer),A
	CALL spawn_bomb
	RET

; Choose a rotating formation column, then use its lowest surviving alien.
; The bomb begins in the empty character cell immediately below that alien.
spawn_bomb:
	; Try all six columns beginning with bomb_column, which rotates after every
	; attempt. This distributes fire instead of always choosing the first alien.
	LD B,6

spawn_try_column:
	; Start at the bottom-row entry for this column, then step back by six bytes
	; until the lowest surviving alien is found.
	LD A,(bomb_column)
	LD C,A
	INC A
	CP 6
	JR C,store_next_column
	XOR A

store_next_column:
	LD (bomb_column),A
	LD A,C
	LD E,A
	LD D,0
	LD HL,invaders
	ADD HL,DE
	LD DE,12
	ADD HL,DE
	LD A,(HL)
	OR A
	JR NZ,spawn_from_bottom
	DEC HL
	DEC HL
	DEC HL
	DEC HL
	DEC HL
	DEC HL
	LD A,(HL)
	OR A
	JR NZ,spawn_from_middle
	DEC HL
	DEC HL
	DEC HL
	DEC HL
	DEC HL
	DEC HL
	LD A,(HL)
	OR A
	JR NZ,spawn_from_top
	DJNZ spawn_try_column
	RET

spawn_from_bottom:
	LD A,(swarm_y)
	ADD A,5
	JR finish_bomb_spawn

spawn_from_middle:
	LD A,(swarm_y)
	ADD A,3
	JR finish_bomb_spawn

spawn_from_top:
	LD A,(swarm_y)
	INC A

finish_bomb_spawn:
	; Convert formation-relative column C into a screen column. Aliens are spaced
	; three character cells apart, so the conversion is C*3 + swarm_x.
	LD (bomb_y),A
	LD A,C
	LD E,A
	ADD A,A
	ADD A,E
	LD E,A
	LD A,(swarm_x)
	ADD A,E
	LD (bomb_x),A
	LD A,1
	LD (bomb_active),A
	LD A,3
	LD (bomb_delay),A
	CALL draw_bomb
	CALL bomb_sound
	RET

draw_bomb:
	LD A,(bomb_x)
	LD E,A
	LD A,(bomb_y)
	LD D,A
	LD A,BOMB_CHAR
	CALL print_at
	RET

player_hit:
	; Show a temporary explosion, consume one life, and either end the game or
	; respawn the player at the centre.
	XOR A
	LD (bomb_active),A
	LD A,(player_x)
	LD E,A
	LD D,21
	LD A,EXPLODE_CHAR
	CALL print_at
	CALL explosion_sound
	CALL frame_delay
	CALL frame_delay
	LD A,(lives)
	DEC A
	LD (lives),A
	JR NZ,player_survived
	LD A,1
	LD (game_state),A
	RET

player_survived:
	LD A,15
	LD (player_x),A
	CALL draw_player
	RET

; ---------------------------------------------------------------------------
; Alien formation. Each movement erases the old formation, changes direction or
; descends at an edge, flips animation_frame, and paints the surviving aliens.

update_swarm:
	; Movement is timer-driven. The old image is erased before changing position;
	; reaching either horizontal limit reverses direction and descends one row.
	LD A,(move_timer)
	DEC A
	LD (move_timer),A
	RET NZ
	LD A,10
	LD (move_timer),A
	LD A,SPACE_CHAR
	LD (swarm_paint),A
	CALL draw_swarm
	LD A,(swarm_dir)
	CP 1
	JR NZ,swarm_left

swarm_right:
	LD A,(swarm_x)
	INC A
	LD (swarm_x),A
	CP 15
	JR C,paint_swarm
	LD A,0FFh
	LD (swarm_dir),A
	JR swarm_down

swarm_left:
	LD A,(swarm_x)
	DEC A
	LD (swarm_x),A
	CP 1
	JR NZ,paint_swarm
	LD A,1
	LD (swarm_dir),A

swarm_down:
	LD A,(swarm_y)
	INC A
	LD (swarm_y),A
	CP 16
	JR C,paint_swarm
	LD A,1
	LD (game_state),A

paint_swarm:
	LD A,(animation_frame)
	XOR 1
	LD (animation_frame),A
	XOR A
	LD (swarm_paint),A
	CALL draw_swarm
	CALL move_sound
	RET

draw_swarm:
	; Paint or erase all 18 formation slots. swarm_paint is SPACE_CHAR while
	; erasing and zero while drawing the row-specific animated UDG.
	LD HL,invaders
	LD B,3
	LD A,INVADER_TOP
	LD (row_char),A
	LD A,(swarm_y)
	LD D,A

draw_swarm_row:
	; Each logical row contains six aliens separated by two blank character cells.
	LD C,6
	LD A,(swarm_x)
	LD E,A

draw_swarm_column:
	LD A,(HL)
	OR A
	JR Z,draw_swarm_next
	LD A,(swarm_paint)
	OR A
	JR NZ,draw_swarm_char
	LD A,(animation_frame)
	OR A
	LD A,(row_char)
	JR Z,draw_swarm_char
	ADD A,7

draw_swarm_char:
	CALL print_at

draw_swarm_next:
	INC HL
	LD A,E
	ADD A,3
	LD E,A
	DEC C
	JR NZ,draw_swarm_column
	LD A,(row_char)
	INC A
	LD (row_char),A
	INC D
	INC D
	DJNZ draw_swarm_row
	RET

; ---------------------------------------------------------------------------
; Destructible shield bases. Four groups of three cells occupy character row 18.
; A live cell is represented by 1 in bases and becomes 0 after either projectile
; hits it. damage_base reports a collision through base_hit.

draw_bases:
	; Four groups begin at columns 4, 11, 18, and 25. Each group contains three
	; independently destructible cells backed by the 12-byte bases array.
	LD HL,bases
	LD B,4
	LD D,18
	LD E,4

draw_base_group:
	LD C,3

draw_base_cell:
	LD A,(HL)
	OR A
	JR Z,draw_base_next
	LD A,BASE_CHAR
	CALL print_at

draw_base_next:
	INC HL
	INC E
	DEC C
	JR NZ,draw_base_cell
	LD A,E
	ADD A,4
	LD E,A
	DJNZ draw_base_group
	RET

; A contains the projectile column. The matching live shield cell is removed.
damage_base:
	; Search the same four-by-three layout used by draw_bases. collision_x is
	; saved because A is reused while walking and drawing the matching cell.
	LD (collision_x),A
	XOR A
	LD (base_hit),A
	LD HL,bases
	LD B,4
	LD D,18
	LD E,4

damage_base_group:
	LD C,3

damage_base_cell:
	LD A,(collision_x)
	CP E
	JR NZ,damage_base_next
	LD A,(HL)
	OR A
	JR Z,damage_base_next
	XOR A
	LD (HL),A
	LD A,1
	LD (base_hit),A
	LD A,EXPLODE_CHAR
	CALL print_at
	CALL hit_sound
	LD A,SPACE_CHAR
	CALL print_at
	RET

damage_base_next:
	INC HL
	INC E
	DEC C
	JR NZ,damage_base_cell
	LD A,E
	ADD A,4
	LD E,A
	DJNZ damage_base_group
	RET

; ---------------------------------------------------------------------------
; Status, messages and direct screen output

update_status:
	; Rewrite only the numeric fields embedded in the fixed header line.
	LD A,(score)
	LD D,0
	LD E,6
	CALL draw_two_digits
	LD D,0
	LD E,8
	LD A,"0"
	CALL print_at
	LD A,(lives)
	ADD A,"0"
	LD D,0
	LD E,18
	CALL print_at
	LD A,(remaining)
	LD D,0
	LD E,29
	CALL draw_two_digits
	RET

draw_two_digits:
	; Convert an unsigned value below 100 by repeated subtraction. B accumulates
	; tens and A retains units, avoiding a division routine.
	LD B,0

digit_tens:
	CP 10
	JR C,digits_ready
	SUB 10
	INC B
	JR digit_tens

digits_ready:
	LD C,A
	LD A,B
	ADD A,"0"
	CALL print_at
	INC E
	LD A,C
	ADD A,"0"
	CALL print_at
	RET

print_at:
	; Input: A=character code, D=row (0-23), E=column (0-31).
	; The routine preserves BC, DE, and HL for callers and writes both bitmap and
	; attribute memory directly.
	LD (draw_char),A
	LD A,D
	LD (draw_row),A
	LD A,E
	LD (draw_column),A
	PUSH BC
	PUSH DE
	PUSH HL

; Locate the glyph. Normal text uses the ROM font at 3D00h; codes
; 144-154 use the game's UDG table.
	LD A,(draw_char)
	CP INVADER_TOP
	JR NC,locate_udg
	SUB 32
	LD L,A
	LD H,0
	ADD HL,HL
	ADD HL,HL
	ADD HL,HL
	LD DE,03D00h
	ADD HL,DE
	EX DE,HL
	JR locate_screen

locate_udg:
	; Each custom glyph is eight bytes, so (character-144)*8 indexes udg_data.
	SUB INVADER_TOP
	LD L,A
	LD H,0
	ADD HL,HL
	ADD HL,HL
	ADD HL,HL
	LD DE,udg_data
	ADD HL,DE
	EX DE,HL

locate_screen:
; Fetch the top bitmap address for the character row and add the column.
	LD A,(draw_row)
	ADD A,A
	LD L,A
	LD H,0
	LD BC,screen_rows
	ADD HL,BC
	LD C,(HL)
	INC HL
	LD B,(HL)
	LD A,(draw_column)
	LD L,A
	LD H,0
	ADD HL,BC
	LD B,8

copy_glyph:
	; Within one Spectrum character cell, consecutive pixel scanlines are 256
	; bytes apart. Incrementing H advances to the next scanline efficiently.
	LD A,(DE)
	LD (HL),A
	INC DE
	INC H
	DJNZ copy_glyph

; Choose a bright Spectrum colour for each game object. Text is cyan,
; alien rows are yellow/magenta/green, bombs are red, and explosions flash.
	LD A,(draw_char)
	CP SPACE_CHAR
	JR Z,attribute_white
	CP INVADER_TOP
	JR C,attribute_cyan
	JR Z,attribute_yellow
	CP INVADER_MID
	JR Z,attribute_magenta
	CP INVADER_LOW
	JR Z,attribute_green
	CP PLAYER_CHAR
	JR Z,attribute_cyan
	CP LASER_CHAR
	JR Z,attribute_white
	CP BOMB_CHAR
	JR Z,attribute_red
	CP EXPLODE_CHAR
	JR Z,attribute_explosion
	CP INVADER_TOP_ALT
	JR Z,attribute_yellow
	CP INVADER_MID_ALT
	JR Z,attribute_magenta
	CP INVADER_LOW_ALT
	JR Z,attribute_green
	CP BASE_CHAR
	JR Z,attribute_cyan

attribute_explosion:
	LD A,0C6h
	JR store_attribute

attribute_yellow:
	LD A,046h
	JR store_attribute

attribute_magenta:
	LD A,043h
	JR store_attribute

attribute_green:
	LD A,044h
	JR store_attribute

attribute_cyan:
	LD A,045h
	JR store_attribute

attribute_red:
	LD A,042h
	JR store_attribute

attribute_white:
	LD A,047h

store_attribute:
	; Attribute memory is linear: 32 bytes per character row starting at 5800h.
	LD (draw_attribute),A
	LD A,(draw_row)
	LD L,A
	LD H,0
	ADD HL,HL
	ADD HL,HL
	ADD HL,HL
	ADD HL,HL
	ADD HL,HL
	LD DE,05800h
	ADD HL,DE
	LD A,(draw_column)
	LD E,A
	LD D,0
	ADD HL,DE
	LD A,(draw_attribute)
	LD (HL),A
	POP HL
	POP DE
	POP BC
	RET

print_text:
	; Print a zero-terminated string from HL, advancing horizontally from D/E.
	LD A,(HL)
	OR A
	RET Z
	CALL print_at
	INC HL
	INC E
	JR print_text

clear_screen:
	; Clear 6144 bitmap bytes, then initialize all 768 attribute cells to bright
	; white ink on black paper.
	LD HL,04000h
	LD BC,01800h

clear_bitmap:
	LD (HL),0
	INC HL
	DEC BC
	LD A,B
	OR C
	JR NZ,clear_bitmap
	LD HL,05800h
	LD BC,00300h

clear_attributes:
	LD (HL),047h
	INC HL
	DEC BC
	LD A,B
	OR C
	JR NZ,clear_attributes
	RET

frame_delay:
	; A small CPU-speed delay controls the main loop without relying on ROM calls
	; or interrupts (the game runs with interrupts disabled).
	LD C,20

frame_delay_outer:
	LD B,0

frame_delay_inner:
	DJNZ frame_delay_inner
	DEC C
	JR NZ,frame_delay_outer
	RET

; ---------------------------------------------------------------------------
; 48K beeper effects. Bit 4 of port FEh drives the speaker; bits 0-2 remain zero
; so playing a sound never changes the black border.

fire_sound:
	; Decreasing pulse delays create a short rising laser sweep.
	PUSH BC
	LD C,120

fire_sound_pulse:
	LD B,C

fire_sound_delay:
	DJNZ fire_sound_delay
	LD A,(sound_phase)
	XOR 10h
	LD (sound_phase),A
	OUT (0FEh),A
	DEC C
	JR NZ,fire_sound_pulse
	XOR A
	LD (sound_phase),A
	OUT (0FEh),A
	POP BC
	RET

hit_sound:
	; Fixed-width pulses produce a harsher impact tone.
	PUSH BC
	LD C,48

hit_sound_loop:
	LD B,60

hit_sound_delay:
	DJNZ hit_sound_delay
	LD A,(sound_phase)
	XOR 10h
	LD (sound_phase),A
	OUT (0FEh),A
	DEC C
	JR NZ,hit_sound_loop
	XOR A
	LD (sound_phase),A
	OUT (0FEh),A
	POP BC
	RET

explosion_sound:
	; A longer sequence of irregular pulse widths gives destroyed invaders and
	; player hits a rough, descending explosion instead of the short impact beep.
	PUSH BC
	LD C,96

explosion_sound_pulse:
	LD A,C
	AND 31
	ADD A,24
	LD B,A

explosion_sound_delay:
	DJNZ explosion_sound_delay
	LD A,(sound_phase)
	XOR 10h
	LD (sound_phase),A
	OUT (0FEh),A
	DEC C
	JR NZ,explosion_sound_pulse
	XOR A
	LD (sound_phase),A
	OUT (0FEh),A
	POP BC
	RET

move_sound:
	; Low, short pulses accompany each completed formation step.
	PUSH BC
	LD C,12

move_sound_pulse:
	LD B,110

move_sound_delay:
	DJNZ move_sound_delay
	LD A,(sound_phase)
	XOR 10h
	LD (sound_phase),A
	OUT (0FEh),A
	DEC C
	JR NZ,move_sound_pulse
	XOR A
	LD (sound_phase),A
	OUT (0FEh),A
	POP BC
	RET

bomb_sound:
	; A separate mid-pitch burst marks each alien bomb launch.
	PUSH BC
	LD C,24

bomb_sound_pulse:
	LD B,75

bomb_sound_delay:
	DJNZ bomb_sound_delay
	LD A,(sound_phase)
	XOR 10h
	LD (sound_phase),A
	OUT (0FEh),A
	DEC C
	JR NZ,bomb_sound_pulse
	XOR A
	LD (sound_phase),A
	OUT (0FEh),A
	POP BC
	RET

show_game_over:
	; Replace the playfield with the defeat and restart prompts.
	CALL clear_screen
	LD D,9
	LD E,10
	LD HL,game_over_text
	CALL print_text
	LD D,12
	LD E,4
	LD HL,restart_text
	CALL print_text
	JR wait_restart

show_win:
	; Replace the playfield with the victory and restart prompts.
	CALL clear_screen
	LD D,9
	LD E,11
	LD HL,you_win_text
	CALL print_text
	LD D,12
	LD E,4
	LD HL,restart_text
	CALL print_text

wait_restart:
	; Fire restarts through start so every array and counter is initialized again.
	CALL frame_delay
	CALL key_fire
	JR NZ,wait_restart
	JP start

; ---------------------------------------------------------------------------
; Text, mutable game state, collision work values, and renderer work values.
; Boolean values use zero for inactive/dead and one for active/alive.

header_text:
	DB "SCORE 000   LIVES 3   ALIENS 18",0

help_text:
	DB "5/8 MOVE       0 FIRE",0

game_over_text:
	DB "GAME OVER",0

you_win_text:
	DB "YOU WIN!",0

restart_text:
	DB "PRESS 0 TO PLAY AGAIN",0

player_x:
	DB 15
lives:
	DB 3
score:
	DB 0
game_state:
	DB 0
player_delay:
	DB 0
laser_active:
	DB 0
laser_x:
	DB 0
laser_y:
	DB 0
laser_delay:
	DB 0
bomb_active:
	DB 0
bomb_x:
	DB 0
bomb_y:
	DB 0
bomb_delay:
	DB 0
bomb_timer:
	DB 35
bomb_column:
	DB 0
swarm_x:
	DB 2
swarm_y:
	DB 4
swarm_dir:
	DB 1
swarm_paint:
	DB 0
move_timer:
	DB 10
remaining:
	DB 18
row_char:
	DB INVADER_TOP
animation_frame:
	DB 0
base_hit:
	DB 0
collision_x:
	DB 0
draw_char:
	DB 0
draw_row:
	DB 0
draw_column:
	DB 0
draw_attribute:
	DB 047h
sound_phase:
	DB 0

invaders:
	; Row-major alive flags: top six, middle six, then bottom six.
	DB 1,1,1,1,1,1
	DB 1,1,1,1,1,1
	DB 1,1,1,1,1,1

bases:
	; Row-major shield flags: four groups of three cells.
	DB 1,1,1,1,1,1
	DB 1,1,1,1,1,1

; Top pixel address of each of the Spectrum's 24 character rows. Spectrum bitmap
; scanlines are interleaved, so print_at uses this table before stepping H.
screen_rows:
	DW 04000h,04020h,04040h,04060h,04080h,040A0h,040C0h,040E0h
	DW 04800h,04820h,04840h,04860h,04880h,048A0h,048C0h,048E0h
	DW 05000h,05020h,05040h,05060h,05080h,050A0h,050C0h,050E0h
; Eleven 8x8 UDG bitmaps, mapped to character codes 144 through 154:
; three primary aliens, player, laser, bomb, explosion, three alternate aliens,
; and one shield-base cell.
udg_data:
	DB 018h,03Ch,07Eh,0DBh,0FFh,024h,05Ah,0A5h
	DB 018h,07Eh,0DBh,0FFh,03Ch,066h,0C3h,042h
	DB 03Ch,07Eh,0FFh,0DBh,07Eh,024h,042h,081h
	DB 018h,03Ch,07Eh,0FFh,0FFh,07Eh,024h,024h
	DB 018h,018h,018h,018h,018h,018h,018h,018h
	DB 024h,018h,024h,018h,024h,018h,024h,018h
	DB 081h,042h,024h,018h,018h,024h,042h,081h
	DB 018h,03Ch,07Eh,0DBh,0FFh,042h,024h,05Ah
	DB 018h,07Eh,0DBh,0FFh,03Ch,0C3h,066h,024h
	DB 03Ch,07Eh,0FFh,0DBh,07Eh,042h,024h,081h
	DB 03Ch,07Eh,0FFh,0FFh,0FFh,0E7h,0C3h,0C3h
