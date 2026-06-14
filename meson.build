project(
  'slangfx',
  'c',
  version: '0.1.0',
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
# slangfx reads raw RGBA from stdin and writes raw RGBA to stdout; demux,
# decode, encode and mux are delegated to ffmpeg via pipes (see
# wrappers/slangfx.py for the orchestration). ffmpeg is therefore not a
# build-time dependency — only a runtime one.
#
# Vulkan SDK on Windows ships everything but no pkg-config file. We probe
# the VULKAN_SDK env var directly and fall back to pkg-config on Linux /
# macOS.
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
    # On Windows: prefer the shared-library import (shaderc_shared.dll).
    # The static shaderc_combined.lib is 200+ MB of optimized C++ which
    # takes minutes to link with MinGW's ld.bfd; the shared variant links
    # in seconds and the DLL is a small runtime dep we ship next to the
    # binary.
    shaderc_lib = 'shaderc_shared'

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

slangfx_sources = files(
  'src/main.c',
  'src/slangp.c',
  'src/slang_compile.c',
  'src/slang_pipeline.c',
  'src/spv_reflect.c',
  'src/slang_metrics.c',
  'src/frame_io.c',
)

slangfx_inc = include_directories('src')

slangfx_deps = []
slangfx_args = []
if get_option('enable_gpu')
  slangfx_deps += [vulkan_dep, shaderc_dep]
  slangfx_args += ['-DSLANGFX_GPU=1']
endif

# Standalone executable. Reads raw RGBA from stdin, writes raw RGBA to stdout.
# See src/main.c for the I/O protocol; wrappers/slangfx.py orchestrates the
# usual ffmpeg-decode | slangfx | ffmpeg-encode pipeline.
slangfx_exe = executable(
  'slangfx',
  slangfx_sources,
  include_directories: slangfx_inc,
  dependencies: slangfx_deps,
  c_args: slangfx_args,
  install: true,
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

test_slangp_parse = executable(
  'test_slangp_parse',
  'tests/test_slangp_parse.c',
  'src/slangp.c',
  include_directories: slangfx_inc,
  build_by_default: false,
)

test('slangp parser', test_slangp_parse,
  workdir: meson.current_source_dir() / 'tests',
)
