; SM-T290 present: one invocation per pixel of the 768x1024 picture.
;   src  1536x2048 BGRA (iPad frame), read at (2x, 2y)
;   dst  800x1280 RGB888 splash, bytes R,B,G, picture at (16,128),
;        scanned out 229 px to the right with wrap (one row up)
; c0.xy = src address, c0.zw = dst address
; c1.x = 800 (panel width), c1.y = 2400 (panel stride), c1.z = 12288 (2 src rows)
; grid: (12, 1024) groups of 64 x 1
@localsize 64, 1, 1
@const(c0.x) 0.0, 0.0, 0.0, 0.0
@const(c1.x) 0.0, 0.0, 0.0, 0.0
@wgid(r10.x)
@invocationid(r0.x)

; x = wg.x * 64 + lid.x, y = wg.y
shl.b r1.x, r10.x, 6
mov.u32u32 r1.y, r10.y
mov.u32u32 r11.x, c1.x
mov.u32u32 r11.y, c1.y
(rpt3)nop
add.u r1.x, r1.x, r0.x
(rpt3)nop
; src = c0.xy + y * 12288 + x * 8
shl.b r2.x, r1.x, 3
mul.u24 r2.y, r1.y, c1.z
(rpt3)nop
add.u r2.x, r2.x, r2.y
(rpt3)nop
add.u r3.x, c0.x, r2.x
mov.u32u32 r3.y, c0.y
(rpt5)nop
ldg.u32 r4.x, g[r3.x], 1
; screen position: bx = x + 16 - 229, by = y + 128; wrap: bx += 800, by -= 1
sub.u r5.x, r1.x, 213
add.u r5.y, r1.y, 128
(rpt3)nop
cmps.s.lt r6.x, r5.x, 0
(rpt3)nop
mad.u24 r5.x, r6.x, r11.x, r5.x
sub.u r5.y, r5.y, r6.x
(rpt3)nop
mul.u24 r7.x, r5.x, 3
(rpt3)nop
mad.u24 r7.x, r5.y, r11.y, r7.x
(rpt3)nop
add.u r8.x, c0.z, r7.x
mov.u32u32 r8.y, c0.w
(sy)nop
; BGRA word: B = bits 0-7, G = 8-15, R = 16-23 -> bytes R, B, G
shr.b r9.x, r4.x, 16
shr.b r9.y, r4.x, 8
(rpt3)nop
; byte stores take half registers
cov.u32u16 hr0.x, r9.x
cov.u32u16 hr0.y, r4.x
cov.u32u16 hr0.z, r9.y
(rpt5)nop
stg.u8 g[r8.x], hr0.x, 1
stg.u8 g[r8.x+1], hr0.y, 1
stg.u8 g[r8.x+2], hr0.z, 1
end
nop
