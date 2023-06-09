# Copyright © 2022 Alyssa Rosenzweig

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

libasahi_layout_files = files(
  'layout.c',
  'tiling.c',
)

libasahi_layout = static_library(
  'asahi_layout',
  [libasahi_layout_files],
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux, inc_asahi],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)

if with_tests
  test(
    'asahi_layout_tests',
    executable(
      'asahi_layout_tests',
      files(
        'tests/test-layout.cpp',
        'tests/test-compression.cpp',
      ),
      c_args : [c_msvc_compat_args, no_override_init_args],
      gnu_symbol_visibility : 'hidden',
      include_directories : [inc_include, inc_src],
      dependencies: [idep_gtest, idep_mesautil],
      link_with : [libasahi_layout],
    ),
    suite : ['asahi'],
    protocol : gtest_test_protocol,
  )
endif
