project(
  'vf-slang',
  'c',
  version: '0.0.1',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
  meson_version: '>=1.1.0',
)

cc       = meson.get_compiler('c')
host_sys = host_machine.system()

# MinGW's default printf is the legacy MSVCRT one — no %zu, no %lld in some
# variants. The standard MinGW idiom is to opt into the C99-compliant
# implementation via this define.
add_project_arguments(
  cc.get_supported_arguments('-D__USE_MINGW_ANSI_STDIO=1'),
  language: 'c',
)

# ---------------------------------------------------------------------------
# Dependencies
#
# Phase 0 the binary uses no GPU code yet (the pipeline is an identity copy)
# so Vulkan/shaderc are optional and will become required in Phase 1. ffmpeg
# is intentionally NOT a dependency: vfslang reads raw RGBA from stdin and
# writes raw RGBA to stdout, so demux/decode/encode/mux is delegated to the
# user's existing ffmpeg via pipes (see wrappers/vfslang.py).
#
# Vulkan SDK on Windows: the SDK ships everything but no pkg-config file.
# We probe VULKAN_SDK env var directly and fall back to the system include
# path on Linux/macOS.
# ---------------------------------------------------------------------------

vulkan_sdk = get_option('vulkan_sdk')
if vulkan_sdk == ''
  vulkan_sdk = run_command('python', '-c', 'import os,sys; sys.stdout.write(os.environ.get("VULKAN_SDK", ""))', check: false).stdout()
endif

vulkan_dep = dependency('', required: false)
shaderc_dep = dependency('', required: false)

if get_option('enable_gpu')
  if vulkan_sdk != ''
    vulkan_inc  = include_directories(vulkan_sdk / 'Include')
    vulkan_lib  = (host_sys == 'windows') ? 'vulkan-1' : 'vulkan'
    shaderc_lib = (host_sys == 'windows') ? 'shaderc_combined' : 'shaderc_shared'

    vulkan_dep = declare_dependency(
      include_directories: vulkan_inc,
      dependencies: cc.find_library(vulkan_lib,
        dirs: [vulkan_sdk / 'Lib', vulkan_sdk / 'lib']),
    )
    shaderc_dep = declare_dependency(
      include_directories: vulkan_inc,
      dependencies: cc.find_library(shaderc_lib,
        dirs: [vulkan_sdk / 'Lib', vulkan_sdk / 'lib']),
    )
  else
    vulkan_dep  = dependency('vulkan',  required: true)
    shaderc_dep = dependency('shaderc', required: true)
  endif
endif

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

vfslang_sources = files(
  'src/vf_slang.c',
  'src/slangp.c',
  'src/slang_compile.c',
  'src/slang_pipeline.c',
)

vfslang_inc = include_directories('src')

vfslang_deps = []
vfslang_args = []
if get_option('enable_gpu')
  vfslang_deps += [vulkan_dep, shaderc_dep]
  vfslang_args += ['-DVFSLANG_GPU=1']
endif

# Standalone executable. Reads raw RGBA from stdin, writes raw RGBA to stdout.
# See src/vf_slang.c for the protocol.
vfslang_exe = executable(
  'vfslang',
  vfslang_sources,
  include_directories: vfslang_inc,
  dependencies: vfslang_deps,
  c_args: vfslang_args,
  install: true,
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

test_slangp_parse = executable(
  'test_slangp_parse',
  'tests/test_slangp_parse.c',
  'src/slangp.c',
  include_directories: vfslang_inc,
  build_by_default: false,
)

test('slangp parser', test_slangp_parse,
  workdir: meson.current_source_dir() / 'tests',
)
