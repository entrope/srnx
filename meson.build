project('rinex', 'c',
    version: '0.1',
    default_options : ['warning_level=3', 'werror=true', 'buildtype=release', 'debug=true'])

# python3 is required for the stdin test programs
python = find_program('python3', 'python3', '/usr/bin/python3', '/usr/local/bin/python3', 'python')
simd = import('unstable-simd')
simd_parse = simd.check('parse',
    avx2 : 'lib/parse_avx2.c',
    neon : 'lib/parse_neon.c',
    compiler : meson.get_compiler('c'))
configure_file(configuration : simd_parse[1],
    output : 'rinex_cfg.h')
librinex = library('rinex',
    'lib/crx_parse.c', 'lib/rnx_parse.c',
    'lib/rinex_mmap.c', 'lib/rnx_priv.c', 'lib/rinex_stdio.c', 'lib/rinex_buffer.c',
    'lib/parse_std.c', 'lib/transpose.c', # 'lib/srnx.c',
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

test_parse_errors = executable('test_parse_errors',
    'test/parse_errors.c',
    build_by_default : false,
    dependencies : [libtap],
    install : false,
    link_with : [librinex])

test_rnx_v2 = executable('test_rnx_v2',
    'test/rinex_v2.c',
    build_by_default : false,
    dependencies : [libtap],
    install : false,
    link_with : [librinex])

test_rnx_v3 = executable('test_rnx_v3',
    'test/rinex_v3.c',
    build_by_default : false,
    dependencies : [libtap],
    install : false,
    link_with : [librinex])

test_transpose = executable('test_transpose',
    'test/test_transpose.c',
    build_by_default : false,
    install : false,
    link_with : [librinex])

benchmark('transpose', test_transpose, args: ['-bench'])
benchmark('transpose_std', test_transpose, args: ['-bench'],
    env: ['RINEX_SIMD=std'])

test('parse_errors', test_parse_errors, protocol: 'tap')
test('rnx_v2', test_rnx_v2, protocol: 'tap')
test('rnx_v2_std', test_rnx_v2, protocol: 'tap',
    env: ['RINEX_SIMD=std'])
test('rnx_v2_in_01', python, depends: rinex_scan,
    args: [meson.current_source_dir() + '/test/stdin.py',
        rinex_scan.full_path(),
        meson.current_source_dir() + '/test/test_01.20o',
        '--mmap',
        'nosuchfile',
        meson.current_source_dir() + '/test/test_02.rnx',
        meson.current_source_dir() + '/test/test_03.20o',
        '--stdio',
        '-v',
        meson.current_source_dir() + '/test/test_04.20o'])
test('rnx_v3', test_rnx_v3, protocol: 'tap')
test('rnx_v3_std', test_rnx_v3, protocol: 'tap',
    env: ['RINEX_SIMD=std'])
test('transpose', test_transpose)
test('transpose_truth', test_transpose, args: ['-truth', '-test'])
