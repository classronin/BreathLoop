cmake_minimum_required(VERSION 3.25)
project(BreathLoop LANGUAGES CXX)
enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 窗口与字体：仍由 vcpkg（glfw3、freetype）提供；libvips 那条链改走共享前缀。
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(freetype CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# libvips 本体走**共享前缀** E:\lib\libvips-BreathLoop：只含 libvips 自己的
# 头文件（include/vips）、静态库（lib/vips.lib、vips-cpp.lib）和 vips*.pc。
# 目的：别的工程重装 vcpkg 的 libvips 时不会动到本工程用的这一份。
# libvips 的依赖链（glib/gobject/gio、cairo/pango/rsvg、jpeg/png/tiff/webp/
# lcms、pcre2/iconv/expat…）仍取自 vcpkg —— 这些包不会因 libvips 重装而变化。
# ---------------------------------------------------------------------------
set(BL_VIPS_PREFIX "E:/lib/libvips-BreathLoop" CACHE PATH "共享 libvips 本体前缀")
if(NOT EXISTS "${BL_VIPS_PREFIX}/include/vips/vips.h")
    message(FATAL_ERROR
        "缺少共享 libvips 前缀：${BL_VIPS_PREFIX}/include/vips/vips.h")
endif()

find_path(VIPS_INCLUDE_DIR vips/vips.h
          PATHS "${BL_VIPS_PREFIX}/include" NO_DEFAULT_PATH REQUIRED)

set(VCPKG_PREFIX "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
find_path(GLIB_INCLUDE_DIR glib.h
          PATHS "${VCPKG_PREFIX}/include/glib-2.0" NO_DEFAULT_PATH REQUIRED)
find_path(GLIB_CONFIG_INCLUDE_DIR glibconfig.h
          PATHS "${VCPKG_PREFIX}/lib/glib-2.0/include" NO_DEFAULT_PATH REQUIRED)

# libvips 的 pkg-config 依赖闭包（vips.pc 的 Requires 展开），逐个解析：
# 不引入 pkg-config 这个外部工具，链接表本身就是唯一出处。
function(breathloop_lib out)
    cmake_parse_arguments(ARG "" "" "NAMES" ${ARGN})
    find_library(${out} NAMES ${ARG_NAMES} REQUIRED)
    set(${out} "${${out}}" PARENT_SCOPE)
endfunction()

macro(breathloop_vips_lib)
    find_library(VIPS_LIB NAMES vips
                 PATHS "${BL_VIPS_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
endmacro()
breathloop_vips_lib()

breathloop_lib(GLIB_LIB      NAMES glib-2.0)
breathloop_lib(GIO_LIB       NAMES gio-2.0)
breathloop_lib(GOBJECT_LIB   NAMES gobject-2.0)
breathloop_lib(GMODULE_LIB   NAMES gmodule-2.0)
breathloop_lib(INTL_LIB      NAMES intl)
breathloop_lib(ICONV_LIB     NAMES iconv)
breathloop_lib(PCRE2_LIB     NAMES libpcre2-8 pcre2-8)
breathloop_lib(EXPAT_LIB     NAMES libexpatMT expat)
breathloop_lib(JPEG_LIB      NAMES libjpeg jpeg)
breathloop_lib(PNG_LIB       NAMES libpng16 libpng)
breathloop_lib(TIFF_LIB      NAMES libtiff tiff)
breathloop_lib(LCMS_LIB      NAMES lcms2)
breathloop_lib(WEBP_LIB      NAMES libwebp webp)
breathloop_lib(WEBPMUX_LIB   NAMES libwebpmux webpmux)
breathloop_lib(WEBPDEMUX_LIB NAMES libwebpdemux webpdemux)
breathloop_lib(ZLIB_LIB      NAMES zlib zlibstatic)
breathloop_lib(FFI_LIB       NAMES ffi)
breathloop_lib(LZMA_LIB      NAMES lzma liblzma)
breathloop_lib(SHARPYUV_LIB  NAMES libsharpyuv sharpyuv)
# SVG 载入（vips[rsvg]）：librsvg 自己是 cairo + pango + gdk-pixbuf + libxml2 的一串。
breathloop_lib(RSVG_LIB      NAMES rsvg-2)
breathloop_lib(CAIRO_LIB     NAMES cairo)
breathloop_lib(CAIRO_GOBJECT_LIB NAMES cairo-gobject)
breathloop_lib(PANGOCAIRO_LIB NAMES pangocairo-1.0)
breathloop_lib(PANGO_LIB     NAMES pango-1.0)
breathloop_lib(PANGOFT2_LIB  NAMES pangoft2-1.0)
breathloop_lib(PANGOWIN32_LIB NAMES pangowin32-1.0)
breathloop_lib(GDKPIXBUF_LIB NAMES gdk_pixbuf-2.0)
breathloop_lib(HARFBUZZ_LIB  NAMES harfbuzz)
breathloop_lib(FONTCONFIG_LIB NAMES fontconfig)
breathloop_lib(XML2_LIB      NAMES libxml2)
breathloop_lib(CROCO_LIB     NAMES croco-0.6)
breathloop_lib(PIXMAN_LIB    NAMES pixman-1)
breathloop_lib(FRIBIDI_LIB   NAMES fribidi)
# 不链 HEIF/HEVC：libvips 以不带 heif 的 feature 安装（见 项目导航.md 第 23 条）。
# 理由：BreathLoop 只处理静态图与 WebP，不需要 HEVC/AV1 解码；而 libde265 的
# init_scan_orders() 是本机唯一被 WER 反复记录到的崩溃点（c0000005）。

# presets.json 是默认值的唯一出处：构建时原样嵌进二进制（raw string），
# 运行期不再去磁盘找它，单 exe 才成立。
file(READ "${CMAKE_SOURCE_DIR}/resources/presets.json" PRESETS_JSON)
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
file(WRITE "${GENERATED_DIR}/presets_embedded.h"
"// 由 CMake 从 resources/presets.json 生成，不要手改。
#pragma once
inline constexpr const char kPresetsJson[] = R\"BRTHLOOP(${PRESETS_JSON})BRTHLOOP\";
")

add_executable(BreathLoop WIN32
    resources/app.rc
    src/main.cpp
    src/bootstrap.cpp
    src/buildcheck.cpp
    src/io/paths.cpp
    src/io/blob.cpp
    src/render/gl.cpp
    src/render/imgtool.cpp
    src/render/gfx.cpp
    src/render/shaders.cpp
    src/render/mask.cpp
    src/render/cutout.cpp
    src/render/eraser.cpp
    src/render/assets.cpp
    src/render/framing.cpp
    src/render/backdrop.cpp
    src/render/layer_view.cpp
    src/render/exporter.cpp
    src/domain/lasso.cpp
    src/domain/region.cpp
    src/domain/layers.cpp
    src/domain/selection.cpp
    src/domain/seamless.cpp
    src/domain/state.cpp
    src/io/settings.cpp
    src/ui/text.cpp
    src/ui/painter.cpp
    src/ui/widgets.cpp
    src/ui/floating.cpp
    src/ui/overlay.cpp
    src/ui/input.cpp
    src/ui/status.cpp
    src/ui/tool_board.cpp
    src/ui/cursors.cpp
    src/ui/param_panel.cpp
    src/ui/layer_panel.cpp
    src/ui/scene.cpp
)

target_include_directories(BreathLoop PRIVATE
    "${VIPS_INCLUDE_DIR}"
    "${VCPKG_PREFIX}/include"
    "${GLIB_INCLUDE_DIR}"
    "${GLIB_CONFIG_INCLUDE_DIR}"
    "${GENERATED_DIR}"
    "${CMAKE_SOURCE_DIR}/third_party"
    "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(BreathLoop PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(BreathLoop PRIVATE /utf-8 /W4 /permissive- /EHsc)
    # 链接图：交付前的体积归因取证（out/linkmap 之外，构建目录里也留一份）
    target_link_options(BreathLoop PRIVATE "/MAP:${CMAKE_BINARY_DIR}/BreathLoop.map")
endif()

target_link_libraries(BreathLoop PRIVATE
    glfw
    OpenGL::GL
    Freetype::Freetype
    Shell32
    "${VIPS_LIB}" "${GLIB_LIB}" "${GIO_LIB}" "${GOBJECT_LIB}" "${GMODULE_LIB}"
    "${INTL_LIB}" "${ICONV_LIB}" "${PCRE2_LIB}" "${EXPAT_LIB}"
    "${JPEG_LIB}" "${PNG_LIB}" "${TIFF_LIB}" "${LCMS_LIB}"
    "${WEBP_LIB}" "${WEBPMUX_LIB}" "${WEBPDEMUX_LIB}" "${ZLIB_LIB}"
    "${FFI_LIB}" "${LZMA_LIB}" "${SHARPYUV_LIB}"
    "${RSVG_LIB}" "${PANGOCAIRO_LIB}" "${PANGO_LIB}" "${PANGOFT2_LIB}"
    "${PANGOWIN32_LIB}" "${CAIRO_LIB}" "${CAIRO_GOBJECT_LIB}"
    "${GDKPIXBUF_LIB}" "${HARFBUZZ_LIB}" "${FONTCONFIG_LIB}"
    "${XML2_LIB}" "${CROCO_LIB}" "${PIXMAN_LIB}" "${FRIBIDI_LIB}"
    ws2_32 winmm userenv ole32 shlwapi version iphlpapi dnsapi gdiplus dwrite bcrypt
)

set_target_properties(BreathLoop PROPERTIES
    OUTPUT_NAME "BreathLoop"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 成品位：链接成功后把 exe 复制到工程根目录（atomic 替换，先写 .new 再换名）。
# 链接过程中的 ${CMAKE_BINARY_DIR}/bin/BreathLoop.exe 可能正被写，双击它等于
# 双击半截镜像（表现：没有窗口、后台满核）。根目录那份只在链接完成后才更新，
# 是唯一适合双击的入口，也是交付件。
add_custom_command(TARGET BreathLoop POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:BreathLoop>"
            "${CMAKE_SOURCE_DIR}/BreathLoop.exe.new"
    COMMAND "${CMAKE_COMMAND}" -E rename
            "${CMAKE_SOURCE_DIR}/BreathLoop.exe.new"
            "${CMAKE_SOURCE_DIR}/BreathLoop.exe"
    COMMENT "成品位：BreathLoop.exe（链接完成后原子替换）"
)
