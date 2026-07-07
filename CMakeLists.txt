cmake_minimum_required(VERSION 3.10)
project(usrp_adsb C)

# Force standard C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find System-Installed Libraries
find_package(UHD REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# Compile libmodes as a static library first. This keeps it separate from my own code.
add_library(modes_static STATIC
    third_party/libmodes/mode-s.c
)

# Tell the compiler where to find "mode-s.h" so the src/ code can #include it
target_include_directories(modes_static PUBLIC third_party/libmodes)
target_link_libraries(modes_static PRIVATE m)

# Compile the application
add_executable(adsb_rx
    src/main.c
    src/rx.c
    src/decode.c
    src/ring_buffer.c
    src/radar_state.c
    src/cpr_math.c
    src/export.c
)

# Tell the executable where to find the <uhd.h> header files
target_include_directories(adsb_rx PRIVATE 
    ${UHD_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

# Enable strict warnings only for my own code, not for libmodes
target_compile_options(adsb_rx PRIVATE 
    -Wall
    -Wextra
    -pedantic
    # Apply -O3 only when building in Release mode
    $<$<CONFIG:Release>:-O3>
    # Apply -O0 (no optimization) and -g (debug symbols) in Debug mode
    $<$<CONFIG:Debug>:-O0 -g>
)

# Link everything together.
# m = the standard C math library (libm), required because libmodes uses sqrt() and atan2()
target_link_libraries(adsb_rx 
    PRIVATE 
    ${UHD_LIBRARIES} 
    Threads::Threads
    modes_static
    ${CURL_LIBRARIES}
)