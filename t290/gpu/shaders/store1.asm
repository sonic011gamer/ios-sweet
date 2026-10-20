@localsize 1, 1, 1
@const(c0.x) 0.0, 0.0, 0.0, 0.0
mov.u32u32 r1.x, c0.x
mov.u32u32 r1.y, c0.y
mov.u32u32 r2.x, 0xc0de
(rpt5)nop
stg.u32 g[r1.x], r2.x, 1
end
nop
