cmake_minimum_required(VERSION 3.22)
project(stronghold_gog LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(PORT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BOOST "${PORT_ROOT}/reference/boost_1_84_0")
set(INNO "${PORT_ROOT}/reference/innoextract")
set(BZIP "${PORT_ROOT}/reference/bzip2")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${PORT_ROOT}/reference/xz" xz EXCLUDE_FROM_ALL)
add_library(bzip2 STATIC "${BZIP}/blocksort.c" "${BZIP}/huffman.c" "${BZIP}/crctable.c"
    "${BZIP}/randtable.c" "${BZIP}/compress.c" "${BZIP}/decompress.c" "${BZIP}/bzlib.c")
target_compile_definitions(bzip2 PRIVATE _FILE_OFFSET_BITS=64)

file(GLOB FILESYSTEM "${BOOST}/libs/filesystem/src/*.cpp")
list(FILTER FILESYSTEM EXCLUDE REGEX "windows_file_codecvt.cpp$")
file(GLOB OPTIONS "${BOOST}/libs/program_options/src/*.cpp")
add_library(boost_support STATIC ${FILESYSTEM} ${OPTIONS}
    "${BOOST}/libs/iostreams/src/zlib.cpp" "${BOOST}/libs/iostreams/src/bzip2.cpp")
target_include_directories(boost_support PUBLIC "${BOOST}" "${BZIP}")
target_compile_definitions(boost_support PRIVATE BOOST_FILESYSTEM_SOURCE BOOST_FILESYSTEM_STATIC_LINK BOOST_FILESYSTEM_NO_CXX20_ATOMIC_REF
    BOOST_FILESYSTEM_DISABLE_COPY_FILE_RANGE BOOST_FILESYSTEM_DISABLE_STATX BOOST_FILESYSTEM_DISABLE_GETRANDOM)
target_link_libraries(boost_support PUBLIC bzip2 z)

foreach(FEATURE ISATTY IOCTL TIMEGM GMTIME_R UTIMENSAT AT_FDCWD UTIMES BUILTIN_BSWAP16 BUILTIN_BSWAP32
        BUILTIN_BSWAP64 ALIGNOF STD_CODECVT_UTF8_UTF16 STD_UNIQUE_PTR ARC4 LZMA BUILTIN_CONV
        POSIX_SPAWNP UNISTD_ENVIRON FORK EXECVP WAITPID)
    set(INNOEXTRACT_HAVE_${FEATURE} 1)
endforeach()
configure_file("${INNO}/src/configure.hpp.in" "configure.hpp")
file(READ "${INNO}/LICENSE" LICENSE_TEXT)
string(REPLACE "\\" "\\\\" LICENSE_TEXT "${LICENSE_TEXT}")
string(REPLACE "\"" "\\\"" LICENSE_TEXT "${LICENSE_TEXT}")
string(REPLACE "\n" "\\n\"\n\"" LICENSE_TEXT "${LICENSE_TEXT}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/release.cpp"
    "#include \"release.hpp\"\nconst char innoextract_name[]=\"innoextract\";\n"
    "const char innoextract_version[]=\"1.9 (Stronghold Android build)\";\n"
    "const char innosetup_versions[]=\"1.2.10 to 6.0.5\";\n"
    "const char innoextract_bugs[]=\"https://github.com/dscharrer/innoextract/issues\";\n"
    "const char innoextract_copyright[]=\"Copyright (C) 2011-2020 Daniel Scharrer\";\n"
    "const char innoextract_license[]=\"${LICENSE_TEXT}\";\n")
file(GLOB_RECURSE INNO_SOURCES "${INNO}/src/*.cpp")
list(FILTER INNO_SOURCES EXCLUDE REGEX "/(util/windows|cli/debug).cpp$")
set_source_files_properties("${INNO}/src/cli/main.cpp" PROPERTIES COMPILE_DEFINITIONS "main=inno_main")
add_executable(innoextract ${INNO_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/release.cpp" inspect.cpp)
target_include_directories(innoextract PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${INNO}/src" "${PORT_ROOT}/reference/xz/src/liblzma/api")
target_link_libraries(innoextract PRIVATE boost_support liblzma)
target_link_options(innoextract PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
set_target_properties(innoextract PROPERTIES OUTPUT_NAME libinnoextract SUFFIX .so)

add_library(stronghold_xz SHARED ../unpack/unpack.c)
target_include_directories(stronghold_xz PRIVATE "${PORT_ROOT}/reference/xz/src/liblzma/api")
target_link_libraries(stronghold_xz PRIVATE liblzma android)
target_compile_options(stronghold_xz PRIVATE -Wall -Wextra -fvisibility=hidden)
target_link_options(stronghold_xz PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384" "-Wl,--no-undefined" "-Wl,--exclude-libs,ALL")
