/*
 * ir3asm: assemble an ir3 compute shader for the a5xx ISA without a GPU.
 *
 * Mesa's computerator assembles only after opening a DRM device; the tablet's
 * GPU is reached through kgsl instead, so this keeps just the assembler:
 * a compiler for gpu_id 506 (same ISA as the A504), ir3_parse_asm(), the
 * binary to OUT.bin and the variant numbers a5xx compute state needs.
 *
 *   ir3asm IN.asm OUT.bin
 *
 * Build: copied into mesa/src/freedreno/computerator and added as an
 * executable next to computerator (see build-ir3asm.sh).
 */
#include <err.h>
#include <stdio.h>
#include <string.h>

#include "common/freedreno_dev_info.h"
#include "ir3/ir3_assembler.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_shader.h"

int
main(int argc, char **argv)
{
   if (argc != 3)
      errx(1, "usage: %s IN.asm OUT.bin", argv[0]);

   struct fd_dev_id id = {};
   id.gpu_id = 506;
   const struct fd_dev_info *info = fd_dev_info_raw(&id);
   if (!info)
      errx(1, "no device info for a506");
   struct ir3_compiler_options opts = {};
   struct ir3_compiler *c = ir3_compiler_create(NULL, &id, info, &opts);
   if (!c)
      errx(1, "ir3_compiler_create failed");

   FILE *in = fopen(argv[1], "r");
   if (!in)
      err(1, "%s", argv[1]);
   struct ir3_kernel_info kinfo;
   memset(&kinfo, 0, sizeof(kinfo));
   struct ir3_shader *sh = ir3_parse_asm(c, &kinfo, in);
   if (!sh)
      errx(1, "assembler failed");
   struct ir3_shader_variant *v = sh->variants;

   FILE *out = fopen(argv[2], "wb");
   if (!out || fwrite(v->bin, 1, v->info.size, out) != v->info.size)
      err(1, "%s", argv[2]);
   fclose(out);

   printf("size %u bytes (%u dwords), instrlen %u, constlen %u\n", v->info.size,
          v->info.sizedwords, v->instrlen, v->constlen);
   printf("max_reg %d, max_half_reg %d, local_size %u %u %u, branchstack %u\n",
          v->info.max_reg, v->info.max_half_reg, v->local_size[0], v->local_size[1],
          v->local_size[2], v->branchstack);
   printf("num_bufs %u\n", kinfo.num_bufs);
   ir3_shader_disasm(v, (uint32_t *)v->bin, stdout);
   return 0;
}
