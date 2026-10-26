# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT

dzn_entrypoints = custom_target(
  'dzn_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['dzn_entrypoints.h', 'dzn_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'dzn',
    '--beta', with_vulkan_beta.to_string()
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

dzn_drirc = custom_target(
  'dzn_drirc',
  input  : [files('dzn_drirc_gen.py'), drirc_gen, '00-dzn-defaults.conf'],
  output : ['dzn_drirc.c', 'dzn_drirc.h'],
  command : [
    prog_python, '@INPUT0@',
    '--import-path', join_paths(dir_source_root, 'src/util'),
    '--drirc-src', '@OUTPUT0@',
    '--drirc-hdr', '@OUTPUT1@',
    '--validate', '@INPUT2@',
  ],
)

libdzn_files = files(
  'dzn_cmd_buffer.c',
  'dzn_descriptor_set.c',
  'dzn_device.c',
  'dzn_dxcore.cpp',
  'dzn_image.c',
  'dzn_meta.c',
  'dzn_nir.c',
  'dzn_pipeline.c',
  'dzn_query.c',
  'dzn_sync.c',
  'dzn_util.c',
  'dzn_wsi.c',
)

dzn_deps = [
  idep_libdxil_compiler,
  idep_libspirv_to_dxil,
  idep_nir,
  idep_nir_headers,
  idep_vulkan_util,
  idep_vulkan_runtime,
  idep_vulkan_wsi,
  idep_mesautil,
  dep_dxheaders,
]

dzn_flags = [ ]
dzn_cpp_flags = [ ]

dzn_link_args = []

if with_platform_windows
  libdzn_files += files('dzn_dxgi.c')
endif

if cc.get_argument_syntax() != 'msvc'
  dzn_flags_to_try = [
    '-Werror=unused-variable',
    '-Werror=unused-but-set-variable',
    '-Werror=unused-value',
    '-Werror=format',
    '-Werror=switch',
  ]
  dzn_flags += cc.get_supported_arguments(dzn_flags_to_try)
  dzn_cpp_flags = cpp.get_supported_arguments(['-Wno-error=unused-value'])
endif

libvulkan_dzn = shared_library(
  'vulkan_dzn',
  [libdzn_files, dzn_entrypoints, dzn_drirc, sha1_h],
  vs_module_defs : vulkan_api_def,
  include_directories : [inc_include, inc_src, inc_util],
  dependencies : [dzn_deps, idep_vulkan_wsi, idep_xmlconfig],
  c_args : dzn_flags,
  cpp_args : dzn_cpp_flags,
  gnu_symbol_visibility : 'hidden',
  link_args : [ld_args_bsymbolic, ld_args_gc_sections, dzn_link_args],
  name_prefix : host_machine.system() == 'windows' ? '' : [],
  install : true,
)

icd_file_name = libname_prefix + 'vulkan_dzn.' + libname_suffix


icd_command = [
   prog_python, '@INPUT0@',
   '--api-version', '1.3', '--xml', '@INPUT1@',
   '--sizeof-pointer', sizeof_pointer,
   '--icd-lib-path', vulkan_icd_lib_path,
   '--icd-filename', icd_file_name,
   '--out', '@OUTPUT@',
]

icd_dev_command = [
   prog_python, '@INPUT0@',
   '--api-version', '1.3', '--xml', '@INPUT1@',
   '--sizeof-pointer', sizeof_pointer,
   '--icd-lib-path', meson.current_build_dir(),
   '--icd-filename', icd_file_name,
   '--out', '@OUTPUT@',
]

if with_platform_windows
  icd_command += '--use-backslash'
  icd_dev_command += '--use-backslash'
endif

dzn_icd = custom_target(
  'dzn_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'dzn_icd.' + vulkan_manifest_suffix,
  command : icd_command,
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install_tag : 'runtime',
  install : true,
)

_dev_icdname = 'dzn_devenv_icd.@0@.json'.format(host_machine.cpu())
_dev_icd = custom_target(
  'dzn_devenv_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : icd_dev_command,
  build_by_default : true,
)

devenv.append('VK_DRIVER_FILES', _dev_icd.full_path())

if with_tests and with_platform_windows and prog_glslang.found()
  large_point_shaders = []
  foreach stage : ['vert', 'frag']
    large_point_shaders += custom_target('large_points_' + stage,
      input : 'tests/large_points.' + stage,
      output : 'large_points.' + stage + '.spv',
      command : [prog_glslang, '-V', '--target-env', 'vulkan1.2', '@INPUT@', '-o', '@OUTPUT@'])
  endforeach
  large_points_test = executable('dzn_large_points_test',
    'tests/large_points_test.cpp', include_directories : inc_include)
  test('dzn_large_points', large_points_test,
       args : [libvulkan_dzn.full_path()] + large_point_shaders,
       depends : libvulkan_dzn, suite : 'dzn', timeout : 120)
  float_control_shaders = []
  foreach mode : [4459, 4460, 4462]
    float_control_shaders += custom_target(
      'float_controls_' + mode.to_string(),
      input : 'tests/float_controls.comp',
      output : 'float_controls_' + mode.to_string() + '.spv',
      command : [prog_glslang, '-V', '--target-env', 'vulkan1.2',
                 '-DMODE=' + mode.to_string(), '@INPUT@', '-o', '@OUTPUT@'],
    )
  endforeach
  float_controls_test = executable('dzn_float_controls_test',
    'tests/float_controls_test.cpp', include_directories : inc_include)
  test('dzn_float_controls', float_controls_test,
       args : [libvulkan_dzn.full_path()] + float_control_shaders,
       depends : libvulkan_dzn, suite : 'dzn', timeout : 120)
  point_test_shaders = []
  foreach stage : ['vert', 'tesc', 'geom', 'frag']
    point_test_shaders += custom_target(
      'point_size_' + stage,
      input : 'tests/point_size.' + stage,
      output : 'point_size.' + stage + '.spv',
      command : [prog_glslang, '-V', '--target-env', 'vulkan1.2', '@INPUT@', '-o', '@OUTPUT@'],
    )
  endforeach
  foreach domain : ['triangles', 'quads', 'isolines']
    foreach spacing : ['equal_spacing', 'fractional_even_spacing', 'fractional_odd_spacing']
      foreach point_mode : [false, true]
        point_test_shaders += custom_target(
          'point_size_tes_' + domain + spacing + point_mode.to_string(),
          input : 'tests/point_size.tese',
          output : 'point_size_' + domain + spacing + point_mode.to_string() + '.tese.spv',
          command : [prog_glslang, '-V', '--target-env', 'vulkan1.2',
                     '-DDOMAIN=' + domain, '-DSPACING=' + spacing] +
                    (point_mode ? ['-DPOINT_MODE=1'] : []) +
                    ['@INPUT@', '-o', '@OUTPUT@'],
        )
      endforeach
    endforeach
  endforeach
  point_pipeline_test = executable('dzn_point_size_pipeline_test',
    'tests/point_size_pipeline_test.cpp', include_directories : inc_include)
  test('dzn_point_size_pipelines', point_pipeline_test,
       args : [libvulkan_dzn.full_path()] + point_test_shaders,
       depends : libvulkan_dzn,
       suite : 'dzn', timeout : 120)
endif
