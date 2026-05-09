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
  meson_version: '>=0.60.0',
)

cc = meson.get_compiler('c')

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

vulkan_dep = dependency('vulkan', required: true)

# shaderc — runtime GLSL/HLSL → SPIR-V compilation. Ships with the Vulkan SDK
# on Windows; available as `libshaderc-dev` on Debian/Ubuntu, `shaderc` on
# Arch, and via Homebrew on macOS.
shaderc_dep = dependency('shaderc', required: true)

# ffmpeg libs (we register an external libavfilter plugin).
avfilter_dep = dependency('libavfilter', required: true, version: '>=8.0.0')
avutil_dep   = dependency('libavutil',   required: true, version: '>=57.0.0')

deps = [vulkan_dep, shaderc_dep, avfilter_dep, avutil_dep]

# ---------------------------------------------------------------------------
# Source layout
# ---------------------------------------------------------------------------

vfslang_sources = files(
  'src/vf_slang.c',
  'src/slangp.c',
  'src/slang_compile.c',
  'src/slang_pipeline.c',
)

vfslang_inc = include_directories('src')

# Built as a shared library that ffmpeg picks up via its plugin path or
# LD_PRELOAD. When the filter is upstreamed this becomes part of
# libavfilter directly.
vfslang_lib = shared_library(
  'vfslang',
  vfslang_sources,
  include_directories: vfslang_inc,
  dependencies: deps,
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
  dependencies: [],
  build_by_default: false,
)

test('slangp parser', test_slangp_parse,
  workdir: meson.current_source_dir() / 'tests',
)
