/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>

#include "dxil_spirv_nir.h"
#include "dxil_nir.h"
#include "nir_builder.h"

static void
check(bool condition, const char *message)
{
   if (!condition) {
      fprintf(stderr, "PointSize: %s\n", message);
      abort();
   }
}

/* The PointSize output is read back to compute another output. Replacing the
 * read with 1 changes that output even if the final raster point size is 1. */
static nir_shader *
make_shader(mesa_shader_stage stage, nir_shader_compiler_options *options)
{
   nir_builder b = nir_builder_init_simple_shader(stage, options, "point_size_readback");
   nir_shader *nir = b.shader;
   if (stage == MESA_SHADER_TESS_CTRL)
      nir->info.tess.tcs_vertices_out = 3;
   if (stage == MESA_SHADER_GEOMETRY) {
      nir->info.gs.input_primitive = MESA_PRIM_POINTS;
      nir->info.gs.output_primitive = MESA_PRIM_POINTS;
      nir->info.gs.vertices_in = nir->info.gs.vertices_out = 1;
      nir->info.gs.invocations = 1;
      nir->info.gs.active_stream_mask = 1;
   }
   const struct glsl_type *type = stage == MESA_SHADER_TESS_CTRL ?
      glsl_array_type(glsl_float_type(), 3, 0) : glsl_float_type();
   nir_variable *size = nir_create_variable_with_location(nir, nir_var_shader_out,
      VARYING_SLOT_PSIZ, type);
   nir_variable *result = nir_create_variable_with_location(nir, nir_var_shader_out,
      VARYING_SLOT_VAR0, type);
   nir_variable *position = stage == MESA_SHADER_GEOMETRY ?
      nir_create_variable_with_location(nir, nir_var_shader_out,
                                        VARYING_SLOT_POS, glsl_vec4_type()) : NULL;
   nir_deref_instr *size_deref = nir_build_deref_var(&b, size);
   nir_deref_instr *result_deref = nir_build_deref_var(&b, result);
   if (stage == MESA_SHADER_TESS_CTRL) {
      nir_def *index = nir_load_invocation_id(&b);
      size_deref = nir_build_deref_array(&b, size_deref, index);
      result_deref = nir_build_deref_array(&b, result_deref, index);
   }
   nir_store_deref(&b, size_deref, nir_imm_float(&b, 2.0f), 1);
   nir_store_deref(&b, result_deref, nir_load_deref(&b, size_deref), 1);
   nir_store_deref(&b, size_deref, nir_imm_float(&b, 1.0f), 1);
   if (stage == MESA_SHADER_GEOMETRY) {
      nir_store_var(&b, position,
                    nir_vec4(&b, nir_imm_float(&b, 0), nir_imm_float(&b, 0),
                             nir_imm_float(&b, 0.5f), nir_imm_float(&b, 1)), 0xf);
      nir_emit_vertex(&b, .stream_id = 0);
      nir_end_primitive(&b, .stream_id = 0);
   }
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   return nir;
}

static void
check_readback(nir_shader *nir, float expected)
{
   /* TCS readback is indexed by InvocationId, so it cannot be folded to a
    * scalar constant by these local optimizations. The other stages can. */
   if (nir->info.stage == MESA_SHADER_TESS_CTRL)
      return;
   for (unsigned i = 0; i < 4; i++) {
      nir_opt_copy_prop_vars(nir);
      nir_lower_vars_to_ssa(nir);
      nir_opt_constant_folding(nir);
      nir_opt_dce(nir);
   }
   bool found = false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_variable *var = nir_intrinsic_get_var(intr, 0);
            if (var && var->data.mode == nir_var_shader_out &&
                var->data.location == VARYING_SLOT_VAR0) {
               check(nir_src_is_const(intr->src[1]), "readback result is not constant");
               check(nir_src_as_float(intr->src[1]) == expected,
                     "PointSize readback changed the computed output value");
               found = true;
            }
         }
      }
   }
   check(found, "readback result disappeared");
}

int main(void)
{
   glsl_type_singleton_init_or_ref();
   nir_shader_compiler_options options;
   dxil_get_nir_compiler_options(&options, SHADER_MODEL_6_0, 32 | 64, 32 | 64);
   for (mesa_shader_stage stage = MESA_SHADER_VERTEX;
        stage <= MESA_SHADER_GEOMETRY; stage++) {
      for (unsigned preserve = 0; preserve < 2; preserve++) {
         nir_shader *nir = make_shader(stage, &options);
         struct dxil_spirv_runtime_conf conf = {
            .fixed_point_size = true,
            .preserve_point_size = preserve,
            .shader_model_max = SHADER_MODEL_6_0,
         };
         struct dxil_spirv_metadata metadata = {0};
         dxil_spirv_nir_passes(nir, &conf, &metadata);
         nir_validate_shader(nir, "PointSize preservation");
         check(!!nir_find_variable_with_location(nir, nir_var_shader_out,
                    VARYING_SLOT_PSIZ) == !!preserve,
               "pre-raster PointSize preservation differs from configuration");
         check_readback(nir, preserve ? 2.0f : 1.0f);
         ralloc_free(nir);
      }
   }

   nir_shader *expanded = make_shader(MESA_SHADER_GEOMETRY, &options);
   struct dxil_spirv_runtime_conf expand_conf = {
      .fixed_point_size = false,
      .preserve_point_size = true,
      .runtime_data_cbv = { .register_space = 31, .base_shader_register = 0 },
      .shader_model_max = SHADER_MODEL_6_0,
   };
   struct dxil_spirv_metadata expand_metadata = {0};
   dxil_spirv_nir_passes(expanded, &expand_conf, &expand_metadata);
   check(expanded->info.gs.output_primitive == MESA_PRIM_TRIANGLE_STRIP,
         "large point was not converted to a triangle strip");
   check(expanded->info.gs.vertices_out == 4,
         "large point expansion has the wrong vertex bound");
   check(nir_find_variable_with_location(expanded, nir_var_shader_out,
                                         VARYING_SLOT_PNTC) != NULL,
         "large point expansion does not provide PointCoord");
   unsigned emits = 0;
   nir_foreach_function_impl(impl, expanded) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_intrinsic &&
                nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_emit_vertex)
               emits++;
         }
      }
   }
   check(emits == 4, "large point expansion does not emit four corners");
   ralloc_free(expanded);

   nir_shader *tcs = make_shader(MESA_SHADER_TESS_CTRL, &options);
   nir_builder tes_builder = nir_builder_init_simple_shader(
      MESA_SHADER_TESS_EVAL, &options, "point_size_signature");
   nir_shader *tes = tes_builder.shader;
   check(tcs->info.outputs_read & VARYING_BIT_PSIZ, "TCS readback was not detected");
   dxil_nir_pad_tes_input_signature(tes, tcs);
   nir_variable *input = nir_find_variable_with_location(tes, nir_var_shader_in,
      VARYING_SLOT_PSIZ);
   check(input != NULL, "TES signature misses preserved TCS PointSize readback");
   check(glsl_get_length(input->type) == 3, "TES PointSize input has wrong patch size");
   ralloc_free(tcs);
   ralloc_free(tes);
   glsl_type_singleton_decref();
   puts("PointSize preservation: 8 stage configurations and TCS/TES signature passed");
   return 0;
}
