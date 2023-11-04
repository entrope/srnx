project('rinex', 'c',
    version: '0.1',
    default_options : ['warning_level=3', 'werror=true', 'buildtype=release', 'debug=true'])

simd = import('unstable-simd')
simd_parse = simd.check('parse',
    avx2 : 'lib/parse_avx2.c',
    neon : 'lib/parse_neon.c',
    compiler : meson.get_compiler('c'))
configure_file(configuration : simd_parse[1],
    output : 'rinex_cfg.h')
librinex = library('rinex',
    'lib/rinex_mmap.c', 'lib/rinex_p.c', 'lib/rinex_parse.c', 'lib/rinex_stdio.c',
    'lib/rinex_buffer.c', 'lib/parse_std.c', 'lib/transpose.c', # 'lib/srnx.c'
    link_with : simd_parse[0]
    )
libtap = declare_dependency(
    include_directories : 'libtap',
    sources : 'libtap/tap.c')

rinex_analyze = executable('rinex_analyze',
    'bin/rinex_analyze.c', 'bin/driver.c',
    install : false,
    link_args : '-flto',
    link_with : [librinex])

rinex_maxima = executable('rinex_maxima',
    'bin/rinex_maxima.c', 'bin/driver.c',
    install : false,
    link_with : [librinex])

rinex_n_obs = executable('rinex_n_obs',
    'bin/rinex_n_obs.c', 'bin/driver.c',
    install : false,
    link_with : [librinex])

rinex_scan = executable('rinex_scan',
    'bin/rinex_scan.c', 'bin/driver.c',
    install : false,
    link_with : [librinex])

test_rnx_v2 = executable('test_rnx_v2',
    'bin/test_rnx_v2.c',
    build_by_default : false,
    dependencies : [libtap],
    install : false,
    link_with : [librinex])

test_rnx_v3 = executable('test_rnx_v3',
    'bin/test_rnx_v3.c',
    build_by_default : false,
    dependencies : [libtap],
    install : false,
    link_with : [librinex])

test_transpose = executable('test_transpose',
    'bin/test_transpose.c',
    build_by_default : false,
    install : false,
    link_with : [librinex])

benchmark('transpose', test_transpose, args: ['-bench'])
benchmark('transpose_std', test_transpose, args: ['-bench'],
    env: ['RINEX_SIMD=std'])

test('rnx_v2', test_rnx_v2, protocol: 'tap')
test('rnx_v2_std', test_rnx_v2, protocol: 'tap',
    env: ['RINEX_SIMD=std'])
test('rnx_v3', test_rnx_v3, protocol: 'tap')
test('rnx_v3_std', test_rnx_v3, protocol: 'tap',
    env: ['RINEX_SIMD=std'])
test('transpose', test_transpose)
test('transpose_truth', test_transpose, args: ['-truth', '-test'])
