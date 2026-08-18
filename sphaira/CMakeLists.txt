cmake_minimum_required(VERSION 3.13)

# generic options.
option(ENABLE_NVJPG "" OFF)
option(ENABLE_NSZ "enables exporting to nsz" ON)

# lib options.
option(ENABLE_LIBUSBHSFS "enables FAT/exFAT hdd mounting" ON)
option(ENABLE_LIBUSBDVD "enables cd/dvd/iso/cue mounting" ON)
option(ENABLE_FTPSRV "enables MTP server support" ON)
option(ENABLE_LIBHAZE "enables MTP server support" ON)

# audio options.
option(ENABLE_AUDIO_MP3 "" ON)
option(ENABLE_AUDIO_OGG "" ON)
option(ENABLE_AUDIO_WAV "" ON)
option(ENABLE_AUDIO_FLAC "" ON)

# devoptab options.
option(ENABLE_DEVOPTAB_HTTP "" ON)
option(ENABLE_DEVOPTAB_NFS "" ON)
option(ENABLE_DEVOPTAB_SMB2 "" ON)
option(ENABLE_DEVOPTAB_FTP "" ON)
option(ENABLE_DEVOPTAB_WEBDAV "" ON)
# disable by default because we are CPU bound for upload/download.
# max speed is 8MiB/s, which is fine for wifi, but awful for ethernet.
# other clients get 36-40MiB/s.
# it also adds 230k to binary size, and i don't think anyone will use it.
option(ENABLE_DEVOPTAB_SFTP "" OFF)

set(sphaira_VERSION 1.0.6)

project(sphaira
    VERSION ${sphaira_VERSION}
    LANGUAGES C CXX
)

find_package(Git REQUIRED)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" symbolic-ref --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_DIRTY
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if (DEFINED sphaira_VERSION_OVERRIDE)
    set(sphaira_DISPLAY_VERSION "${sphaira_VERSION_OVERRIDE} [${GIT_COMMIT}]")
else()
    set(sphaira_DISPLAY_VERSION "${sphaira_VERSION} [${GIT_COMMIT}]")
endif()

add_executable(sphaira
    source/ui/menus/appstore.cpp
    source/ui/menus/file_viewer.cpp
    source/ui/menus/image_viewer.cpp
    source/ui/menus/filebrowser.cpp
    source/ui/menus/file_picker.cpp
    source/ui/menus/homebrew.cpp
    source/ui/menus/irs_menu.cpp
    source/ui/menus/main_menu.cpp
    source/ui/menus/menu_base.cpp
    source/ui/menus/save_menu.cpp
    source/ui/menus/storage_menu.cpp
    source/ui/menus/themezer.cpp
    source/ui/menus/ghdl.cpp
    source/ui/menus/usb_menu.cpp
    source/ui/menus/gc_menu.cpp
    source/ui/menus/game_menu.cpp
    source/ui/menus/game_meta_menu.cpp
    source/ui/menus/game_nca_menu.cpp
    source/ui/menus/grid_menu_base.cpp
    source/ui/menus/install_stream_menu_base.cpp

    source/ui/error_box.cpp
    source/ui/notification.cpp
    source/ui/nvg_util.cpp
    source/ui/option_box.cpp
    source/ui/popup_list.cpp
    source/ui/progress_box.cpp
    source/ui/scrollable_text.cpp
    source/ui/sidebar.cpp
    source/ui/widget.cpp
    source/ui/list.cpp
    source/ui/scrolling_text.cpp
    source/ui/music_player.cpp
    source/ui/forwarder_editor.cpp
    source/ui/steamgriddb_icon.cpp

    source/app.cpp
    source/download.cpp
    source/dumper.cpp
    source/option.cpp
    source/evman.cpp
    source/fs.cpp
    source/image.cpp
    source/location.cpp
    source/log.cpp
    source/main.cpp
    source/nro.cpp
    source/nxlink.cpp
    source/owo.cpp
    source/swkbd.cpp
    source/web.cpp
    source/hasher.cpp
    source/i18n.cpp
    source/threaded_file_transfer.cpp
    source/title_info.cpp
    source/minizip_helper.cpp
    source/msp.cpp
    source/nx_versions.cpp

    source/utils/utils.cpp
    source/utils/audio.cpp
    source/utils/devoptab_common.cpp
    source/utils/devoptab_romfs.cpp
    source/utils/devoptab_save.cpp
    source/utils/devoptab_nro.cpp
    source/utils/devoptab_nca.cpp
    source/utils/devoptab_nsp.cpp
    source/utils/devoptab_xci.cpp
    source/utils/devoptab_zip.cpp
    source/utils/devoptab_bfsar.cpp
    source/utils/devoptab_vfs.cpp
    source/utils/devoptab_fatfs.cpp
    source/utils/devoptab_game.cpp
    source/utils/devoptab_mounts.cpp
    source/utils/devoptab.cpp

    source/usb/base.cpp
    source/usb/usbds.cpp
    source/usb/usbhs.cpp
    source/usb/usb_uploader.cpp
    source/usb/usb_installer.cpp
    source/usb/usb_dumper.cpp

    source/yati/yati.cpp
    source/yati/container/nsp.cpp
    source/yati/container/xci.cpp
    source/yati/source/file.cpp
    source/yati/source/stream.cpp
    source/yati/source/stream_file.cpp

    source/yati/nx/es.cpp
    source/yati/nx/keys.cpp
    source/yati/nx/nca.cpp
    source/yati/nx/ncz.cpp
    source/yati/nx/ncm.cpp
    source/yati/nx/ns.cpp

    source/yati/nx/nxdumptool_rsa.c
    source/yati/nx/nxdumptool/save.c
)

target_compile_definitions(sphaira PRIVATE
    -DAPP_VERSION="${sphaira_VERSION}"
    -DAPP_DISPLAY_VERSION="${sphaira_DISPLAY_VERSION}"
    -DCURL_NO_OLDIES=1
    -DDEV_BUILD=$<BOOL:${DEV_BUILD}>
    -DZSTD_STATIC_LINKING_ONLY=1
)

target_compile_options(sphaira PRIVATE
    -Wall
    -Wextra

    # unsure if it's a good idea to enable these by default as
    # it may cause breakage upon compiler updates.
    # -Werror
    # -Wfatal-errors

    # disabled as nx uses s64 for size and offset, however stl uses size_t instead, thus
    # there being a lot of warnings.
    -Wno-sign-compare
    # disabled as many overriden methods don't use the params.
    -Wno-unused-parameter
    # pedantic warning, missing fields are set to 0.
    -Wno-missing-field-initializers
    # disabled as it warns for strcat 2 paths together, but it will never
    # overflow due to fs enforcing a max path len anyway.
    -Wno-format-truncation
    # many false positives when LTO is not enabled.
    -Wno-suggest-final-types

    # the below are taken from my gba emulator, they've served me well ;)
    -Wformat-overflow=2
    -Wundef
    -Wmissing-include-dirs
    -fstrict-aliasing
    -Wstrict-overflow=2
    -Walloca
    -Wduplicated-cond
    -Wwrite-strings
    -Wdate-time
    -Wlogical-op
    -Wpacked
    -Wcast-qual
    -Wcast-align
    -Wimplicit-fallthrough=5
    -Wuninitialized
)

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

FetchContent_Declare(libpulsar
    GIT_REPOSITORY https://github.com/ITotalJustice/switch-libpulsar.git
    GIT_TAG ac7bc97
)

FetchContent_Declare(nanovg
    GIT_REPOSITORY https://github.com/ITotalJustice/nanovg-deko3d.git
    GIT_TAG 845c9fc
)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 5c20573
)

FetchContent_Declare(yyjson
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG 0.12.0
)

FetchContent_Declare(minIni
    GIT_REPOSITORY https://github.com/ITotalJustice/minIni-nx.git
    GIT_TAG 6e952b6
)

FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    SOURCE_SUBDIR build/cmake
)

FetchContent_Declare(libnxtc
    GIT_REPOSITORY https://github.com/ITotalJustice/libnxtc.git
    GIT_TAG 88ce3d8
    PATCH_COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${CMAKE_SOURCE_DIR}/cmake/patches/libnxtc-libnx-nacp-layout.patch"
)

FetchContent_Declare(nvjpg
    GIT_REPOSITORY https://github.com/ITotalJustice/oss-nvjpg.git
    GIT_TAG 45680e7
)

FetchContent_Declare(dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs.git
    GIT_TAG b962384
    SOURCE_SUBDIR NONE
)

if (ENABLE_NVJPG)
    FetchContent_Declare(nvjpg
        GIT_REPOSITORY https://github.com/ITotalJustice/oss-nvjpg.git
        GIT_TAG 45680e7
    )

    FetchContent_MakeAvailable(nvjpg)

    add_library(nvjpg
        ${nvjpg_SOURCE_DIR}/lib/decoder.cpp
        ${nvjpg_SOURCE_DIR}/lib/image.cpp
        ${nvjpg_SOURCE_DIR}/lib/surface.cpp
    )

    target_include_directories(nvjpg PUBLIC ${nvjpg_SOURCE_DIR}/include)
    set_target_properties(nvjpg PROPERTIES CXX_STANDARD 26)

    target_link_libraries(nvjpg PRIVATE nvjpg)
    target_compile_definitions(sphaira PRIVATE ENABLE_NVJPG)
endif()

if (ENABLE_NSZ)
    target_sources(sphaira PRIVATE source/utils/nsz_dumper.cpp)
    target_compile_definitions(sphaira PRIVATE ENABLE_NSZ)
endif()

if (ENABLE_LIBUSBHSFS)
    # enable this if you want ntfs and ext4 support, at the cost of a huge final binary size.
    set(USBHSFS_GPL OFF)
    set(USBHSFS_SXOS_DISABLE ON)

    FetchContent_Declare(libusbhsfs
        GIT_REPOSITORY https://github.com/ITotalJustice/libusbhsfs.git
        GIT_TAG 625269b
    )

    FetchContent_MakeAvailable(libusbhsfs)

    target_compile_definitions(sphaira PRIVATE ENABLE_LIBUSBHSFS)
    target_link_libraries(sphaira PRIVATE libusbhsfs)
else()
    target_sources(sphaira PRIVATE source/ff16/ffunicode.c)
endif()

if (ENABLE_LIBUSBDVD)
    FetchContent_Declare(libusbdvd
        GIT_REPOSITORY https://github.com/proconsule/libusbdvd.git
        GIT_TAG 3cb0613
        PATCH_COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${CMAKE_SOURCE_DIR}/cmake/patches/libusbdvd-libnx-usb-enums.patch"
    )

    FetchContent_MakeAvailable(libusbdvd)

    add_library(libusbdvd
        ${libusbdvd_SOURCE_DIR}/source/usbdvd.cpp
        ${libusbdvd_SOURCE_DIR}/source/usbdvd_scsi.cpp
        ${libusbdvd_SOURCE_DIR}/source/usbdvd_utils.cpp
        ${libusbdvd_SOURCE_DIR}/source/fs/usbdvd_datadisc.cpp
        ${libusbdvd_SOURCE_DIR}/source/fs/audiocdfs/audiocdfs.cpp
        ${libusbdvd_SOURCE_DIR}/source/fs/audiocdfs/cdaudio_devoptab.cpp
        ${libusbdvd_SOURCE_DIR}/source/fs/iso9660/usbdvd_iso9660.cpp
        ${libusbdvd_SOURCE_DIR}/source/fs/iso9660/iso9660_devoptab.cpp
        ${libusbdvd_SOURCE_DIR}/source/fs/udf/usbdvd_udf.cpp
        ${libusbdvd_SOURCE_DIR}/source/fs/udf/udf_devoptab.cpp
        ${libusbdvd_SOURCE_DIR}/source/os/switch/switch_usb.cpp

    )
    target_include_directories(libusbdvd PRIVATE ${libusbdvd_SOURCE_DIR}/source/)
    target_include_directories(libusbdvd PRIVATE ${libusbdvd_SOURCE_DIR}/source/os/switch)
    target_include_directories(libusbdvd PRIVATE ${libusbdvd_SOURCE_DIR}/source/fs)
    target_include_directories(libusbdvd PRIVATE ${libusbdvd_SOURCE_DIR}/source/fs/audiocdfs)
    target_include_directories(libusbdvd PRIVATE ${libusbdvd_SOURCE_DIR}/source/fs/iso9660)
    target_include_directories(libusbdvd PRIVATE ${libusbdvd_SOURCE_DIR}/source/fs/udf)
    target_include_directories(libusbdvd PUBLIC ${libusbdvd_SOURCE_DIR}/include)

    target_compile_definitions(sphaira PRIVATE ENABLE_LIBUSBDVD)
    target_link_libraries(sphaira PRIVATE libusbdvd)
    target_sources(sphaira PRIVATE source/usbdvd.cpp)
endif()

if (ENABLE_FTPSRV)
    FetchContent_Declare(ftpsrv
        GIT_REPOSITORY https://github.com/ITotalJustice/ftpsrv.git
        GIT_TAG 7c82402
        SOURCE_SUBDIR NONE
        PATCH_COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${CMAKE_SOURCE_DIR}/cmake/patches/ftpsrv-upload-callback.patch"
    )

    FetchContent_MakeAvailable(ftpsrv)

    set(FTPSRV_LIB_BUILD TRUE)
    set(FTPSRV_LIB_VFS_CUSTOM ${ftpsrv_SOURCE_DIR}/src/platform/nx/vfs_nx.h)
    set(FTPSRV_LIB_PATH_SIZE 0x301)
    set(FTPSRV_LIB_SESSIONS 16)
    set(FTPSRV_LIB_BUF_SIZE 1024*64)

    set(FTPSRV_LIB_CUSTOM_DEFINES
        USE_VFS_SAVE=$<BOOL:FALSE>
        USE_VFS_STORAGE=$<BOOL:TRUE>
        # disabled as it may conflict with the gamecard menu.
        USE_VFS_GC=$<BOOL:FALSE>
        USE_VFS_USBHSFS=$<BOOL:FALSE>
        VFS_NX_BUFFER_IO=$<BOOL:TRUE>
        # let sphaira handle init / closing of the hdd.
        USE_VFS_USBHSFS_INIT=$<BOOL:FALSE>
        # disable romfs mounting as otherwise we cannot write / modify sphaira.nro
        USE_VFS_ROMFS=$<BOOL:FALSE>
        FTP_SOCKET_HEADER="${ftpsrv_SOURCE_DIR}/src/platform/nx/socket_nx.h"
    )

    add_subdirectory(${ftpsrv_SOURCE_DIR} binary_dir)

    add_library(ftpsrv_helper
        ${ftpsrv_SOURCE_DIR}/src/platform/nx/vfs_nx.c
        ${ftpsrv_SOURCE_DIR}/src/platform/nx/vfs/vfs_nx_none.c
        ${ftpsrv_SOURCE_DIR}/src/platform/nx/vfs/vfs_nx_root.c
        ${ftpsrv_SOURCE_DIR}/src/platform/nx/vfs/vfs_nx_fs.c
        ${ftpsrv_SOURCE_DIR}/src/platform/nx/vfs/vfs_nx_storage.c
        ${ftpsrv_SOURCE_DIR}/src/platform/nx/utils.c
    )

    target_link_libraries(ftpsrv_helper PUBLIC ftpsrv)
    target_include_directories(ftpsrv_helper PUBLIC ${ftpsrv_SOURCE_DIR}/src/platform)

    target_compile_definitions(sphaira PRIVATE ENABLE_FTPSRV)
    target_link_libraries(sphaira PRIVATE ftpsrv_helper)

    target_sources(sphaira PRIVATE
        source/ftpsrv_helper.cpp
        source/ui/menus/ftp_menu.cpp
    )
endif()

if (ENABLE_LIBHAZE)
    FetchContent_Declare(libhaze
        GIT_REPOSITORY https://github.com/ITotalJustice/libhaze.git
        GIT_TAG 81154c1
        PATCH_COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${CMAKE_SOURCE_DIR}/cmake/patches/libhaze-sphaira.patch"
    )

    FetchContent_MakeAvailable(libhaze)
    target_compile_definitions(libhaze PRIVATE -DSPHAIRA_VERSION="${sphaira_VERSION}")
    target_compile_definitions(sphaira PRIVATE ENABLE_LIBHAZE)
    target_link_libraries(sphaira PRIVATE libhaze)

    target_sources(sphaira PRIVATE
        source/haze_helper.cpp
        source/ui/menus/mtp_menu.cpp
    )
endif()

if (ENABLE_DEVOPTAB_HTTP)
    target_compile_definitions(sphaira PRIVATE ENABLE_DEVOPTAB_HTTP)
    target_sources(sphaira PRIVATE source/utils/devoptab_http.cpp)
endif()

if (ENABLE_DEVOPTAB_NFS)
    FetchContent_Declare(libnfs
        GIT_REPOSITORY https://github.com/ITotalJustice/libnfs.git
        GIT_TAG 65f3e11
    )

    FetchContent_MakeAvailable(libnfs)
    target_compile_definitions(sphaira PRIVATE ENABLE_DEVOPTAB_NFS)
    target_link_libraries(sphaira PRIVATE nfs)
    target_sources(sphaira PRIVATE source/utils/devoptab_nfs.cpp)

    # todo: fix this upstream as nfs should export these folders.
    target_include_directories(sphaira PRIVATE
        ${libnfs_SOURCE_DIR}/include
        ${libnfs_SOURCE_DIR}/include/nfsc
        ${libnfs_SOURCE_DIR}/nfs
    )
endif()

if (ENABLE_DEVOPTAB_SMB2)
    FetchContent_Declare(libsmb2
        GIT_REPOSITORY https://github.com/ITotalJustice/libsmb2.git
        GIT_TAG 867beea
    )

    FetchContent_MakeAvailable(libsmb2)
    target_compile_definitions(sphaira PRIVATE ENABLE_DEVOPTAB_SMB2)
    target_link_libraries(sphaira PRIVATE smb2)
    target_sources(sphaira PRIVATE source/utils/devoptab_smb2.cpp)
endif()

if (ENABLE_DEVOPTAB_FTP)
    target_compile_definitions(sphaira PRIVATE ENABLE_DEVOPTAB_FTP)
    target_sources(sphaira PRIVATE source/utils/devoptab_ftp.cpp)
endif()

if (ENABLE_DEVOPTAB_SFTP)
    # set to build from source, otherwise it will link against the older dkp libssh2.
    if (1)
        set(CRYPTO_BACKEND mbedTLS)
        set(ENABLE_ZLIB_COMPRESSION ON)
        set(ENABLE_DEBUG_LOGGING OFF)
        set(BUILD_EXAMPLES OFF)
        set(BUILD_TESTING OFF)
        set(LINT OFF)

        FetchContent_Declare(libssh2
            GIT_REPOSITORY https://github.com/libssh2/libssh2.git
            # GIT_TAG a0dafb3 # latest commit, works fine, but i'll stick to main release.
            GIT_TAG libssh2-1.11.1
        )

        FetchContent_MakeAvailable(libssh2)
        target_link_libraries(sphaira PRIVATE libssh2::libssh2)
    else()
        include(FindPkgConfig)
        pkg_check_modules(LIBSSH2 libssh2 REQUIRED)
        target_include_directories(sphaira PRIVATE ${LIBSSH2_INCLUDE_DIRS})
        target_link_libraries(sphaira PRIVATE ${LIBSSH2_LIBRARIES})
    endif()

    target_compile_definitions(sphaira PRIVATE ENABLE_DEVOPTAB_SFTP)
    target_sources(sphaira PRIVATE source/utils/devoptab_sftp.cpp)
endif()

if (ENABLE_DEVOPTAB_WEBDAV)
    set(PUGIXML_NO_EXCEPTIONS ON)
    set(PUGIXML_WCHAR_MODE OFF)

    FetchContent_Declare(pugixml
        GIT_REPOSITORY https://github.com/zeux/pugixml.git
        GIT_TAG v1.15
    )

    FetchContent_MakeAvailable(pugixml)
    target_compile_definitions(sphaira PRIVATE ENABLE_DEVOPTAB_WEBDAV)
    target_link_libraries(sphaira PRIVATE pugixml)
    target_sources(sphaira PRIVATE source/utils/devoptab_webdav.cpp)
endif()

if (ENABLE_AUDIO_MP3)
    FetchContent_Declare(id3v2lib
        GIT_REPOSITORY https://github.com/larsbs/id3v2lib.git
        GIT_TAG 141ffb8
    )

    FetchContent_MakeAvailable(id3v2lib)
    target_link_libraries(sphaira PRIVATE id3v2lib)
    target_compile_definitions(sphaira PRIVATE ENABLE_AUDIO_MP3)
endif()

if (ENABLE_AUDIO_OGG)
    target_compile_definitions(sphaira PRIVATE ENABLE_AUDIO_OGG)
endif()

if (ENABLE_AUDIO_WAV)
    target_compile_definitions(sphaira PRIVATE ENABLE_AUDIO_WAV)
endif()

if (ENABLE_AUDIO_FLAC)
    target_compile_definitions(sphaira PRIVATE ENABLE_AUDIO_FLAC)
endif()

# ztsd
set(ZSTD_BUILD_STATIC ON)
set(ZSTD_BUILD_SHARED OFF)
set(ZSTD_BUILD_COMPRESSION ${ENABLE_NSZ})
set(ZSTD_MULTITHREAD_SUPPORT ${ENABLE_NSZ})
set(ZSTD_BUILD_DECOMPRESSION ON)
set(ZSTD_BUILD_DICTBUILDER OFF)
set(ZSTD_LEGACY_SUPPORT OFF)
set(ZSTD_BUILD_PROGRAMS OFF)
set(ZSTD_BUILD_TESTS OFF)

# minini
set(MININI_LIB_NAME minIni)
set(MININI_USE_STDIO ON)
set(MININI_USE_NX OFF)
set(MININI_USE_FLOAT ON)

# nanovg
if (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(NANOVG_DEBUG ON)
endif()
set(NANOVG_NO_JPEG OFF)
set(NANOVG_NO_PNG OFF)
set(NANOVG_NO_BMP OFF)
set(NANOVG_NO_PSD ON)
set(NANOVG_NO_TGA ON)
set(NANOVG_NO_GIF ON)
set(NANOVG_NO_HDR ON)
set(NANOVG_NO_PIC ON)
set(NANOVG_NO_PNM ON)

# yyjson
set(YYJSON_INSTALL OFF)
set(YYJSON_DISABLE_READER OFF)
set(YYJSON_DISABLE_WRITER OFF)
set(YYJSON_DISABLE_UTILS ON)
set(YYJSON_DISABLE_FAST_FP_CONV ON)
set(YYJSON_DISABLE_NON_STANDARD ON)
set(YYJSON_DISABLE_UTF8_VALIDATION ON)
set(YYJSON_DISABLE_UNALIGNED_MEMORY_ACCESS OFF)

FetchContent_MakeAvailable(
    libpulsar
    nanovg
    stb
    minIni
    yyjson
    zstd
    libnxtc
    dr_libs
)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

add_library(dr_libs INTERFACE)
target_include_directories(dr_libs INTERFACE ${dr_libs_SOURCE_DIR})

add_library(libnxtc
    ${libnxtc_SOURCE_DIR}/source/nxtc.c
    ${libnxtc_SOURCE_DIR}/source/nxtc_log.c
    ${libnxtc_SOURCE_DIR}/source/nxtc_utils.c
)
target_include_directories(libnxtc PUBLIC ${libnxtc_SOURCE_DIR}/include)

find_package(ZLIB REQUIRED)
find_library(minizip_lib minizip REQUIRED)
find_path(minizip_inc minizip REQUIRED)

find_package(CURL REQUIRED)
find_path(mbedtls_inc mbedtls REQUIRED)
find_library(mbedcrypto_lib mbedcrypto REQUIRED)

add_library(fatfs
    source/ff16/diskio.c
    source/ff16/ff.c
)

target_include_directories(fatfs PUBLIC source/ff16)

set_target_properties(sphaira PROPERTIES
    C_STANDARD 23
    C_EXTENSIONS ON
    CXX_STANDARD 26
    CXX_EXTENSIONS ON
)

target_link_libraries(sphaira PRIVATE
    libpulsar
    minIni
    nanovg
    stb
    yyjson
    libnxtc
    fatfs
    dr_libs
    libzstd_static

    ${minizip_lib}
    ZLIB::ZLIB
    CURL::libcurl
    ${mbedcrypto_lib}
)

target_include_directories(sphaira PRIVATE
    include
    ${minizip_inc}
    ${mbedtls_inc}
    include/yati/nx/nxdumptool
)

# copy the romfs
file(COPY ${CMAKE_SOURCE_DIR}/assets/romfs DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

# create assets target
dkp_add_asset_target(sphaira_romfs ${CMAKE_CURRENT_BINARY_DIR}/romfs)

file(GLOB_RECURSE sphaira_romfs_source_files CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    ${CMAKE_SOURCE_DIR}/assets/romfs/*
)
add_custom_target(sphaira_romfs_sync
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets/romfs
        ${CMAKE_CURRENT_BINARY_DIR}/romfs
    DEPENDS ${sphaira_romfs_source_files}
)
add_dependencies(sphaira_romfs sphaira_romfs_sync)
set_property(TARGET sphaira_romfs APPEND PROPERTY DKP_ASSET_FILES ${sphaira_romfs_source_files})

# wait until hbl is built first as we need the exefs to embed
add_dependencies(sphaira hbl_nso hbl_npdm)

# set the embed path for assets and hbl
target_compile_options(sphaira PRIVATE
    --embed-dir=${CMAKE_SOURCE_DIR}/assets/embed
    --embed-dir=${CMAKE_BINARY_DIR}/hbl
)

# add nanovg shaders to romfs
dkp_install_assets(sphaira_romfs
    DESTINATION shaders
    TARGETS
        fill_aa_fsh
        fill_fsh
        fill_vsh
)

# create nacp
nx_generate_nacp(
    OUTPUT sphaira.nacp
    NAME ${CMAKE_PROJECT_NAME}
    AUTHOR TotalJustice
    VERSION ${sphaira_VERSION}
)

# create nro
nx_create_nro(sphaira
    OUTPUT ${CMAKE_BINARY_DIR}/sphaira.nro
    ICON ${CMAKE_SOURCE_DIR}/assets/icon.jpg
    NACP sphaira.nacp
    ROMFS sphaira_romfs
)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/switch/sphaira)

add_custom_command(
    TARGET sphaira_nro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_BINARY_DIR}/sphaira.nro
        ${CMAKE_BINARY_DIR}/switch/sphaira/sphaira.nro
)

message(STATUS "generating nro in: ${CMAKE_BINARY_DIR}/sphaira.nro")
message(STATUS "run nxlink -s ${CMAKE_BINARY_DIR}/sphaira.nro")
