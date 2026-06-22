cmake_minimum_required(VERSION 3.10)
project(usrp_adsb C)

# Force standard C99
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 1. Find System-Installed Libraries
find_package(UHD REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# 2. Compile the Third-Party Code
# We compile libmodes as a static library first. This keeps it separate from your code.
add_library(modes_static STATIC
        third_party/libmodes/mode-s.c
)

# Tell the compiler where to find "mode-s.h" so your src/ code can #include it
target_include_directories(modes_static PUBLIC third_party/libmodes)
target_link_libraries(modes_static PRIVATE m)

# 3. Compile Your Application
add_executable(adsb_rx
        src/main.c
        src/rx.c
        src/decode.c
        src/ring_buffer.c
)

# Tell your executable where to find headers for UHD and CURL
target_include_directories(adsb_rx PRIVATE
        ${UHD_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
)

# Enable strict warnings only for my own code, not for libmodes
target_compile_options(adsb_rx PRIVATE
        -Wall
        -Wextra
        -pedantic
)

# 4. Link Everything Together
target_link_libraries(adsb_rx
        PRIVATE
        ${UHD_LIBRARIES}
        Threads::Threads
        ${CURL_LIBRARIES}
        modes_static
)
