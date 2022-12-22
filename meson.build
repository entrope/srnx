project('rinex', 'c',
    version: '0.1',
    default_options : ['warning_level=3', 'werror=true', 'buildtype=release'])

simd = import('unstable-simd')
simd_parse = simd.check('parse',
    avx2 : 'avx2_parse.c',
    neon : 'neon_parse.c',
    compiler : meson.get_compiler('c'))
configure_file(configuration : simd_parse[1],
    output : 'rinex_cfg.h')
librinex = library('rinex',
    'rinex_mmap.c', 'rinex_p.c', 'rinex_parse.c', 'rinex_stdio.c',
    'rinex_buffer.c', 'std_parse.c', 'transpose.c', # 'srnx.o'
    link_with : simd_parse[0]
    )
libtap = declare_dependency(
    include_directories : 'libtap',
    sources : 'libtap/tap.c')

rinex_analyze = executable('rinex_analyze',
    'rinex_analyze.c', 'driver.c',
    install : false,
    link_args : '-flto',
    link_with : [librinex])

rinex_maxima = executable('rinex_maxima',
    'rinex_maxima.c', 'driver.c',
    install : false,
    link_with : [librinex])

rinex_n_obs = executable('rinex_n_obs',
    'rinex_n_obs.c', 'driver.c',
    install : false,
    link_with : [librinex])

rinex_scan = executable('rinex_scan',
    'rinex_scan.c', 'driver.c',
    install : false,
    link_with : [librinex])

test_rnx_v2 = executable('test_rnx_v2',
    'test_rnx_v2.c',
    build_by_default : false,
    dependencies : [libtap],
    install : false,
    link_with : [librinex])

test_rnx_v3 = executable('test_rnx_v3',
    'test_rnx_v3.c',
    build_by_default : false,
    dependencies : [libtap],
    install : false,
    link_with : [librinex])

test_transpose = executable('transpose_test',
    'transpose_test.c',
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
