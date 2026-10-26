/* SPDX-License-Identifier: MIT */
#include "dxil_function.h"
#include "dxil_internal.h"
#include "util/ralloc.h"
#include <stdio.h>
#include <stdlib.h>

static void check(bool condition)
{
   if (!condition) {
      fprintf(stderr, "DXIL atomic overload signature mismatch\n");
      abort();
   }
}

int main(void)
{
   void *context = ralloc_context(NULL);
   struct dxil_module module;
   dxil_module_init(&module, context);
   const enum overload_type overloads[] = {DXIL_I32, DXIL_I64};
   for (unsigned i = 0; i < 2; i++) {
      const struct dxil_type *value_type = dxil_get_overload_type(&module, overloads[i]);
      const struct dxil_type *coord_type = dxil_get_overload_type(&module, DXIL_I32);
      const struct dxil_func *binop = dxil_get_function(&module, "dx.op.atomicBinOp", overloads[i]);
      const struct dxil_func *swap = dxil_get_function(&module, "dx.op.atomicCompareExchange", overloads[i]);
      check(binop && swap);
      check(binop->type->function_def.ret_type == value_type);
      check(swap->type->function_def.ret_type == value_type);
      check(binop->type->function_def.args.num_types == 7);
      check(swap->type->function_def.args.num_types == 7);
      for (unsigned coordinate = 2; coordinate < 5; coordinate++)
         check(swap->type->function_def.args.types[coordinate] == coord_type);
      check(binop->type->function_def.args.types[6] == value_type);
      check(swap->type->function_def.args.types[5] == value_type);
      check(swap->type->function_def.args.types[6] == value_type);
   }
   dxil_module_release(&module);
   ralloc_free(context);
   return 0;
}

