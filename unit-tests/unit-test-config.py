#!python3

# License: Apache 2.0. See LICENSE file in root directory.
# Copyright(c) 2020 Intel Corporation. All Rights Reserved.

#
# Syntax:
#     unit-test-config.py <dir> <build-dir>
#
# Looks for possible single-file unit-testing targets (test-*) in $dir, and builds
# a CMakeLists.txt in $builddir to compile them.
#
# Each target is compiled in its own project, so that each file ends up in a different
# process and so individual tests cannot affect others except through hardware.
#

import sys, os, subprocess, locale, re, getopt
from glob import glob

current_dir = os.path.dirname( os.path.abspath( __file__ ) )
sys.path.append( current_dir + os.sep + "py" )

from rspy import file, repo, libci, log

def usage():
    ourname = os.path.basename(sys.argv[0])
    print( 'Syntax: ' + ourname + ' [options] <dir> <build-dir>' )
    print( '        build unit-testing framework for the tree in $dir' )
    print( '        -r, --regex    configure all tests that fit the following regular expression' )
    print( '        -t, --tag      configure all tests with the following tag. If used multiple times runs all tests matching' )
    print( '                       all tags. e.g. -t tag1 -t tag2 will run tests who have both tag1 and tag2' )
    print( '                       tests automatically get tagged with \'exe\' or \'py\' and based on their location' )
    print( '                       inside unit-tests/, e.g. unit-tests/func/test-hdr.py gets [func, py]' )
    print( '        --list-tags    print out all available tags. This option will not run any tests' )
    print( '        --list-tests   print out all available tests. This option will not run any tests' )
    print( '                       if both list-tags and list-tests are specified each test will be printed along' )
    print( '                       with what tags it has' )
    print( '        --context        The context to use for test configuration' )
    sys.exit(2)

regex = None
required_tags = []
list_tags = False
list_tests = False
context = None
# parse command-line:
try:
    opts, args = getopt.getopt( sys.argv[1:], 'hr:t:',
                                longopts=['help', 'regex=', 'tag=', 'list-tags', 'list-tests', 'context='] )
except getopt.GetoptError as err:
    log.e( err )  # something like "option -a not recognized"
    usage()
for opt, arg in opts:
    if opt in ('-h', '--help'):
        usage()
    elif opt in ('-r', '--regex'):
        regex = arg
    elif opt in ('-t', '--tag'):
        required_tags.append( arg )
    elif opt == '--list-tags':
        list_tags = True
    elif opt == '--list-tests':
        list_tests = True
    elif opt == '--context':
        context = arg

if len( args ) != 2:
    usage()
dir=args[0]
builddir=args[1]
if not os.path.isdir( dir ) or not os.path.isdir( builddir ):
    usage()

# We have to stick to Unix conventions because CMake on Windows is fubar...
root = repo.root.replace( '\\' , '/' )
src = root + '/src'

def generate_cmake( builddir, testdir, testname, filelist, custom_main ):
    makefile = builddir + '/' + testdir + '/CMakeLists.txt'
    log.d( '   creating:', makefile )
    handle = open( makefile, 'w' )
    filelist = '\n    '.join( filelist )
    handle.write( '''
# This file is automatically generated!!
# Do not modify or your changes will be lost!

cmake_minimum_required( VERSION 3.1.0 )
project( ''' + testname + ''' )

set( SRC_FILES ''' + filelist + '''
)
add_executable( ''' + testname + ''' ${SRC_FILES} )
source_group( "Common Files" FILES ${CATCH_FILES} ''' + dir + '''/test.cpp''' )
    if not custom_main:
        handle.write( ' ' + dir + '/unit-test-default-main.cpp' )
    handle.write( ''' )
set_property(TARGET ''' + testname + ''' PROPERTY CXX_STANDARD 11)
target_link_libraries( ''' + testname + ''' ${DEPENDENCIES} )

set_target_properties( ''' + testname + ''' PROPERTIES FOLDER "Unit-Tests/''' + os.path.dirname( testdir ) + '''" )

using_easyloggingpp( ${PROJECT_NAME} SHARED )

# Add the repo root directory (so includes into src/ will be specific: <src/...>)
target_include_directories(''' + testname + ''' PRIVATE ''' + root + ''')

''' )
    handle.close()


def find_include( include, relative_to ):
    """
    Try to match the include to an existing file.

    :param include: the text within "" or <> from the include directive
    :param relative_to: the directory from which to start finding if include is non-absolute
    :return: the normalized & absolute file path, if found -- otherwise, None
    """
    if include:
        if not os.path.isabs( include ):
            include = os.path.normpath( relative_to + '/' + include )
        include = include.replace( '\\', '/' )
        if os.path.exists( include ):
            return include


standard_include_dirs = [
    os.path.join( root, 'include' ),
    os.path.join( root, 'third-party', 'rsutils', 'include' ),
    root
    ]
def find_include_in_dirs( include ):
    """
    Search for the given include in all the standard include directories
    """
    global include_dirs
    for include_dir in standard_include_dirs:
        path = find_include( include, include_dir )
        if path:
            return path


def find_includes( filepath, filelist = set() ):
    """
    Recursively searches a .cpp file for #include directives and returns
    a set of all of them.
    :return: a list of all includes found
    """
    filedir = os.path.dirname(filepath)
    try:
        log.debug_indent()
        for include_line in file.grep( r'^\s*#\s*include\s+("(.*)"|<(.*)>)\s*$', filepath ):
            m = include_line['match']
            index = include_line['index']
            include = find_include( m.group(2), filedir ) or find_include_in_dirs( m.group(2) ) or find_include_in_dirs( m.group(3) )
            if include:
                if include in filelist:
                    log.d( m.group(0), '->', include, '(already processed)' )
                else:
                    log.d( m.group(0), '->', include )
                    filelist.add( include )
                    filelist = find_includes( include, filelist )
            else:
                log.d( 'not found:', m.group(0) )
    finally:
        log.debug_unindent()
    return filelist

def process_cpp( dir, builddir ):
    global regex, required_tags, list_only, available_tags, tests_and_tags
    found = []
    shareds = []
    statics = []
    if regex:
        pattern = re.compile( regex )
    log.d( 'looking for C++ files in:', dir )
    for f in file.find( dir, '(^|/)test-.*\.cpp$' ):
        testdir = os.path.splitext( f )[0]                          # "log/internal/test-all"  <-  "log/internal/test-all.cpp"
        testparent = os.path.dirname(testdir)                       # "log/internal"
        # We need the project name unique: keep the path but make it nicer:
        if testparent:
            testname = 'test-' + testparent.replace( '/', '-' ) + '-' + os.path.basename( testdir )[
                                                                        5:]  # "test-log-internal-all"
        else:
            testname = testdir  # no parent folder so we get "test-all"

        if regex and not pattern.search( testname ):
            continue

        log.d( '... found:', f )
        log.debug_indent()
        try:
            if required_tags or list_tags:
                config = libci.TestConfigFromCpp( dir + os.sep + f, context )
                if not all( tag in config.tags for tag in required_tags ):
                    continue
                available_tags.update( config.tags )
                if list_tests:
                    tests_and_tags[ testname ] = config.tags

            if testname not in tests_and_tags:
                tests_and_tags[testname] = None

            # Build the list of files we want in the project:
            # At a minimum, we have the original file, plus any common files
            filelist = [ dir + '/' + f, '${CATCH_FILES}' ]
            # Add any "" includes specified in the .cpp that we can find
            includes = find_includes( dir + '/' + f )
            # Add any files explicitly listed in the .cpp itself, like this:
            #         //#cmake:add-file <filename>
            # Any files listed are relative to $dir
            shared = False
            static = False
            custom_main = False
            for cmake_directive in file.grep( '^//#cmake:\s*', dir + '/' + f ):
                m = cmake_directive['match']
                index = cmake_directive['index']
                cmd, *rest = cmake_directive['line'][m.end():].split()
                if cmd == 'add-file':
                    for additional_file in rest:
                        files = additional_file
                        if not os.path.isabs( additional_file ):
                            files = dir + '/' + testparent + '/' + additional_file
                        files = glob( files )
                        if not files:
                            log.e( f + '+' + str(index) + ': no files match "' + additional_file + '"' )
                        for abs_file in files:
                            abs_file = os.path.normpath( abs_file )
                            abs_file = abs_file.replace( '\\', '/' )
                            if not os.path.exists( abs_file ):
                                log.e( f + '+' + str(index) + ': file not found "' + additional_file + '"' )
                            log.d( 'add file:', abs_file )
                            filelist.append( abs_file )
                            if( os.path.splitext( abs_file )[0] == 'cpp' ):
                                # Add any "" includes specified in the .cpp that we can find
                                includes |= find_includes( abs_file )
                elif cmd == 'static!':
                    if len(rest):
                        log.e( f + '+' + str(index) + ': unexpected arguments past \'' + cmd + '\'' )
                    elif shared:
                        log.e( f + '+' + str(index) + ': \'' + cmd + '\' mutually exclusive with \'shared!\'' )
                    else:
                        log.d( 'static!' )
                        static = True
                elif cmd == 'shared!':
                    if len(rest):
                        log.e( f + '+' + str(index) + ': unexpected arguments past \'' + cmd + '\'' )
                    elif static:
                        log.e( f + '+' + str(index) + ': \'' + cmd + '\' mutually exclusive with \'static!\'' )
                    else:
                        log.d( 'shared!' )
                        shared = True
                elif cmd == 'custom-main':
                    custom_main = True
                else:
                    log.e( f + '+' + str(index) + ': unknown cmd \'' + cmd + '\' (should be \'add-file\', \'static!\', or \'shared!\')' )
            for include in includes:
                filelist.append( include )

            # all tests use the common test.cpp file
            filelist.append( root + "/unit-tests/test.cpp" )

            # 'cmake:custom-main' indicates that the test is defining its own main() function.
            # If not specified we use a default main() which lives in its own .cpp:
            if not custom_main:
                filelist.append( root + "/unit-tests/unit-test-default-main.cpp" )

            if list_only:
                continue

            # Each CMakeLists.txt sits in its own directory
            os.makedirs( builddir + '/' + testdir, exist_ok=True )  # "build/log/internal/test-all"
            generate_cmake( builddir, testdir, testname, filelist, custom_main )
            if static:
                statics.append( testdir )
            elif shared:
                shareds.append( testdir )
            else:
                found.append( testdir )
        finally:
            log.debug_unindent()
    return found, shareds, statics
def process_py( dir, builddir ):
    # TODO
    return [],[],[]

list_only = list_tags or list_tests
available_tags = set()
tests_and_tags = dict()
normal_tests = []
shared_tests = []
static_tests = []
n,sh,st = process_cpp( dir, builddir )

if list_only:
    if list_tags and list_tests:
        for t in sorted( tests_and_tags.keys() ):
            print( t, "has tags:", ' '.join( tests_and_tags[t] ) )
    #
    elif list_tags:
        for t in sorted( list( available_tags ) ):
            print( t )
    #
    elif list_tests:
        for t in sorted( tests_and_tags.keys() ):
            print( t )
    sys.exit( 0 )

normal_tests.extend( n )
shared_tests.extend( sh )
static_tests.extend( st )
n,sh,st = process_py( dir, builddir )
normal_tests.extend( n )
shared_tests.extend( sh )
static_tests.extend( st )

cmakefile = builddir + '/CMakeLists.txt'
name = os.path.basename( os.path.realpath( dir ))
log.d( 'Creating "' + name + '" project in', cmakefile )

handle = open( cmakefile, 'w' )
handle.write( '''

set( CATCH_FILES
    ''' + dir + '''/catch/catch.hpp
)

''' )

n_tests = 0
for sdir in normal_tests:
    handle.write( 'add_subdirectory( ' + sdir + ' )\n' )
    log.d( '... including:', sdir )
    n_tests += 1
if len(shared_tests):
    handle.write( 'if(NOT ${BUILD_SHARED_LIBS})\n' )
    handle.write( '    message( INFO " ' + str(len(shared_tests)) + ' shared lib unit-tests will be skipped. Check BUILD_SHARED_LIBS to run them..." )\n' )
    handle.write( 'else()\n' )
    for test in shared_tests:
        handle.write( '    add_subdirectory( ' + test + ' )\n' )
        log.d( '... including:', sdir )
        n_tests += 1
    handle.write( 'endif()\n' )
if len(static_tests):
    handle.write( 'if(${BUILD_SHARED_LIBS})\n' )
    handle.write( '    message( INFO " ' + str(len(static_tests)) + ' static lib unit-tests will be skipped. Uncheck BUILD_SHARED_LIBS to run them..." )\n' )
    handle.write( 'else()\n' )
    for test in static_tests:
        handle.write( '    add_subdirectory( ' + test + ' )\n' )
        log.d( '... including:', sdir )
        n_tests += 1
    handle.write( 'endif()\n' )
handle.close()

print( 'Generated ' + str(n_tests) + ' unit-tests' )
if log.n_errors():
    sys.exit(1)
sys.exit(0)

