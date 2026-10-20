; debug: each invocation stores x | y << 16 at dst + (y * 768 + x) * 4
@localsize 64, 1, 1
@const(c0.x) 0.0, 0.0, 0.0, 0.0
@const(c1.x) 0.0, 0.0, 0.0, 0.0
@wgid(r10.x)
@invocationid(r0.x)
shl.b r1.x, r10.x, 6
mov.u32u32 r1.y, r10.y
(rpt3)nop
add.u r1.x, r1.x, r0.x
(rpt3)nop
mov.u32u32 r6.x, 3072
(rpt3)nop
mul.u24 r2.x, r1.y, r6.x
shl.b r2.y, r1.x, 2
shl.b r3.y, r1.y, 16
(rpt3)nop
add.u r2.x, r2.x, r2.y
or.b r4.x, r1.x, r3.y
(rpt3)nop
add.u r5.x, c0.z, r2.x
mov.u32u32 r5.y, c0.w
(rpt5)nop
stg.u32 g[r5.x], r4.x, 1
end
nop
