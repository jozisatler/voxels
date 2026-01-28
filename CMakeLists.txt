cmake_minimum_required(VERSION 3.10)
project(Panda3DSpinningCube)

set(CMAKE_CXX_STANDARD 11)

# --- Find Panda3D Logic for Windows ---
if(WIN32)
    # Check if PANDA3D_ROOT is set, otherwise try to find it
    if(NOT DEFINED PANDA3D_ROOT)
        # Search for C:/Panda3D-1.10.*
        file(GLOB PANDA_DIRS "C:/Panda3D*")
        # Reverse sort to likely get the latest version if multiple exist
        list(SORT PANDA_DIRS)
        list(REVERSE PANDA_DIRS)
        
        if(PANDA_DIRS)
            list(GET PANDA_DIRS 0 PANDA3D_ROOT)
            message(STATUS "Auto-detected Panda3D at: ${PANDA3D_ROOT}")
        else()
             message(FATAL_ERROR "Panda3D SDK not found in default locations. Please run cmake with -DPANDA3D_ROOT=C:/path/to/panda3d")
        endif()
    endif()

    # Set include paths
    include_directories("${PANDA3D_ROOT}/include")
    # Python include is often needed for Panda3D headers
    file(GLOB PYTHON_INCLUDE_DIRS "${PANDA3D_ROOT}/python/include")
    if(PYTHON_INCLUDE_DIRS)
        include_directories(${PYTHON_INCLUDE_DIRS})
    endif()

    # Set library paths
    link_directories("${PANDA3D_ROOT}/lib")

    # Define the libraries to link
    # Note: On Windows MSVC, these link against the Release libraries by default.
    set(PANDA_LIBS
        libp3framework
        libpanda
        libpandaexpress
        libp3dtool
        libp3dtoolconfig
        libp3direct
    )
    
    # Add definitions usually required
    add_definitions(-DWIN32 -D_WINDOWS)

else()
    # Simplified Linux/Unix assumption (pkg-config or standard paths)
    find_package(Panda3D REQUIRED)
    include_directories(${PANDA3D_INCLUDE_DIRS})
    set(PANDA_LIBS ${PANDA3D_LIBRARIES})
endif()

# --- Executable ---
add_executable(SpinningCube main.cpp VoxLoader.cpp)

# Link libraries
# Link libraries
target_link_libraries(SpinningCube ${PANDA_LIBS})

if (MSVC)
    target_compile_options(SpinningCube PRIVATE  /W3 /wd4244 /wd4267 /O2 /Oi /Ot /Oy /GL)
    target_link_options(SpinningCube PRIVATE /LTCG)
else()
    target_compile_options(SpinningCube PRIVATE -Wall -Wno-unknown-pragmas -Wfatal-errors -O3)
endif()

# Copy vox files
add_custom_command(TARGET SpinningCube POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/doom.vox"
    "$<TARGET_FILE_DIR:SpinningCube>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/castle.vox"
    "$<TARGET_FILE_DIR:SpinningCube>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/skybox.sha"
    "$<TARGET_FILE_DIR:SpinningCube>"
)

# Try to copy the necessary DLLs to the build directory (Optional convenience)
if(WIN32 AND EXISTS "${PANDA3D_ROOT}/bin")
    message(STATUS "Configuring to copy DLLs from ${PANDA3D_ROOT}/bin")
    file(GLOB PANDA_DLLS "${PANDA3D_ROOT}/bin/*.dll")
    foreach(DLL ${PANDA_DLLS})
        add_custom_command(TARGET SpinningCube POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLL}"
            "$<TARGET_FILE_DIR:SpinningCube>"
        )
    endforeach()
endif()
