/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "dxil_spirv_nir.h"
#include "spirv_to_dxil.h"
#include "dxil_nir.h"
#include "nir_to_dxil.h"
#include "shader_enums.h"
#include "spirv/nir_spirv.h"
#include "util/blob.h"

#include "git_sha1.h"
#include "vulkan/vulkan.h"

static_assert((mesa_shader_stage)DXIL_SPIRV_SHADER_NONE == MESA_SHADER_NONE, "must match");
static_assert((mesa_shader_stage)DXIL_SPIRV_SHADER_VERTEX == MESA_SHADER_VERTEX, "must match");
static_assert((mesa_shader_stage)DXIL_SPIRV_SHADER_TESS_CTRL == MESA_SHADER_TESS_CTRL, "must match");
static_assert((mesa_shader_stage)DXIL_SPIRV_SHADER_TESS_EVAL == MESA_SHADER_TESS_EVAL, "must match");
static_assert((mesa_shader_stage)DXIL_SPIRV_SHADER_GEOMETRY == MESA_SHADER_GEOMETRY, "must match");
static_assert((mesa_shader_stage)DXIL_SPIRV_SHADER_FRAGMENT == MESA_SHADER_FRAGMENT, "must match");
static_assert((mesa_shader_stage)DXIL_SPIRV_SHADER_COMPUTE == MESA_SHADER_COMPUTE, "must match");
static_assert((mesa_shader_stage)DXIL_SPIRV_SHADER_KERNEL == MESA_SHADER_KERNEL, "must match");

bool
spirv_to_dxil(const uint32_t *words, size_t word_count,
              struct dxil_spirv_specialization *specializations,
              unsigned int num_specializations, dxil_spirv_shader_stage stage,
              const char *entry_point_name,
              enum dxil_validator_version validator_version_max,
              const struct dxil_spirv_debug_options *dgb_opts,
              const struct dxil_spirv_runtime_conf *conf,
              const struct dxil_spirv_logger *logger,
              struct dxil_spirv_object *out_dxil)
{
   if (stage == DXIL_SPIRV_SHADER_NONE || stage == DXIL_SPIRV_SHADER_KERNEL)
      return false;

   glsl_type_singleton_init_or_ref();

   struct nir_to_dxil_options opts = {
      .environment = DXIL_ENVIRONMENT_VULKAN,
      .shader_model_max = conf->shader_model_max,
      .validator_version_max = validator_version_max,
      .tessellation_input_control_points = conf->tessellation_input_control_points,
   };

   const struct spirv_to_nir_options *spirv_opts = dxil_spirv_nir_get_spirv_options();
   nir_shader_compiler_options nir_options;
   const unsigned supported_bit_sizes = 16 | 32 | 64;
   dxil_get_nir_compiler_options(&nir_options, conf->shader_model_max, supported_bit_sizes, supported_bit_sizes);
   // We will manually handle base_vertex when vertex_id and instance_id have
   // have been already converted to zero-base.
   nir_options.lower_base_vertex = conf->first_vertex_and_base_instance_mode != DXIL_SPIRV_SYSVAL_TYPE_ZERO;

   struct nir_spirv_specialization *spec = NULL;
   if (specializations && num_specializations > 0) {
      spec = vtn_alloc_specialization(num_specializations);

      for (unsigned i = 0; i < num_specializations; i++) {
         vtn_add_specialization_entry(spec, i, specializations[i].id,
                                      sizeof(specializations[i].value),
                                      &specializations[i].value, false);
      }
   }
   nir_shader *nir = spirv_to_nir(words, word_count, spec,
                                  (mesa_shader_stage)stage, entry_point_name,
                                  spirv_opts, &nir_options);
   vtn_free_specialization(spec);
   if (!nir) {
      glsl_type_singleton_decref();
      return false;
   }

   if (conf->link_tessellation &&
       (stage == DXIL_SPIRV_SHADER_TESS_CTRL || stage == DXIL_SPIRV_SHADER_TESS_EVAL)) {
      if (conf->tessellation_domain < TESS_PRIMITIVE_TRIANGLES ||
          conf->tessellation_domain > TESS_PRIMITIVE_ISOLINES ||
          conf->tessellation_spacing < TESS_SPACING_EQUAL ||
          conf->tessellation_spacing > TESS_SPACING_FRACTIONAL_EVEN ||
          !conf->tessellation_input_control_points || conf->tessellation_input_control_points > 32 ||
          !conf->tessellation_output_control_points || conf->tessellation_output_control_points > 32) {
         ralloc_free(nir);
         glsl_type_singleton_decref();
         return false;
      }
      nir->info.tess._primitive_mode = conf->tessellation_domain;
      nir->info.tess.spacing = conf->tessellation_spacing;
      nir->info.tess.ccw = conf->tessellation_ccw;
      nir->info.tess.point_mode = conf->tessellation_point_mode;
      nir->info.tess.tcs_vertices_out = conf->tessellation_output_control_points;
   }

   nir_validate_shader(nir,
                       "Validate before feeding NIR to the DXIL compiler");

   dxil_spirv_nir_prep(nir);

   dxil_spirv_nir_passes(nir, conf, &out_dxil->metadata);

   if (conf->link_tessellation) {
      if (!conf->tessellation_peer_words || !conf->tessellation_peer_word_count) {
         ralloc_free(nir);
         glsl_type_singleton_decref();
         return false;
      }
      const mesa_shader_stage peer_stage = stage == DXIL_SPIRV_SHADER_TESS_CTRL ?
         MESA_SHADER_TESS_EVAL : MESA_SHADER_TESS_CTRL;
      nir_shader *peer = spirv_to_nir(conf->tessellation_peer_words,
         conf->tessellation_peer_word_count, NULL, peer_stage, entry_point_name,
         spirv_opts, &nir_options);
      if (!peer) {
         ralloc_free(nir);
         glsl_type_singleton_decref();
         return false;
      }
      peer->info.tess = nir->info.tess;
      struct dxil_spirv_runtime_conf peer_conf = *conf;
      peer_conf.yz_flip.mode = DXIL_SPIRV_YZ_FLIP_NONE;
      struct dxil_spirv_metadata peer_metadata = {0};
      dxil_spirv_nir_prep(peer);
      dxil_spirv_nir_passes(peer, &peer_conf, &peer_metadata);
      struct dxil_spirv_metadata link_metadata = {0};
      nir_shader *tes = stage == DXIL_SPIRV_SHADER_TESS_EVAL ? nir : peer;
      nir_shader *tcs = stage == DXIL_SPIRV_SHADER_TESS_CTRL ? nir : peer;
      dxil_spirv_nir_link(tes, tcs, conf, &link_metadata);
      out_dxil->metadata.requires_runtime_data |= link_metadata.requires_runtime_data;
      ralloc_free(peer);
   }

   if (dgb_opts->dump_nir)
      nir_print_shader(nir, stderr);

   struct dxil_logger logger_inner = {.priv = logger->priv,
                                      .log = logger->log};

   struct blob dxil_blob;
   if (!nir_to_dxil(nir, &opts, &logger_inner, &dxil_blob)) {
      if (dxil_blob.allocated)
         blob_finish(&dxil_blob);
      ralloc_free(nir);
      glsl_type_singleton_decref();
      return false;
   }

   ralloc_free(nir);
   blob_finish_get_buffer(&dxil_blob, &out_dxil->binary.buffer,
                          &out_dxil->binary.size);

   glsl_type_singleton_decref();
   return true;
}

void
spirv_to_dxil_free(struct dxil_spirv_object *dxil)
{
   free(dxil->binary.buffer);
}

size_t
spirv_to_dxil_get_runtime_conf_size(void)
{
   return sizeof(struct dxil_spirv_runtime_conf);
}

uint64_t
spirv_to_dxil_get_version()
{
   const char sha1[] = MESA_GIT_SHA1;
   const char* dash = strchr(sha1, '-');
   if (dash) {
      return strtoull(dash + 1, NULL, 16);
   }
   return 0;
}
