cmake_minimum_required(VERSION 3.20)

project(zxspecemu C RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(zxspecemu
    src/main.c
    src/spectrum.c
    resources.rc
)

target_include_directories(zxspecemu PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_SOURCE_DIR}/third_party/chips>
)
target_link_libraries(zxspecemu PRIVATE gdi32 comdlg32 user32 winmm)

if(MINGW)
    target_link_options(zxspecemu PRIVATE -mwindows)
endif()
