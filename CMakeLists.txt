cmake_minimum_required(VERSION 3.20)
project(MatPacker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MATPACKER_BUILD_GUI "Build the MatPacker editor (requires wxWidgets)" ON)
option(MATPACKER_BUILD_LIBRARY "Build the matpacker shared library" ON)

# Everything but the editor UI: baking, KTX2 and glTF. Shared by the editor and the library.
add_library(MatPackerCore STATIC
    Src/Baker.cpp
    Src/GltfExport.cpp
    Src/ImageSource.cpp
    Src/Json.cpp
    Src/Ktx2.cpp
    Src/ThirdParty/cgltf.cpp
    Src/ThirdParty/stb_image.cpp
    Src/ThirdParty/stb_image_resize2.cpp
    Src/ThirdParty/stb_image_write.cpp
)
set_target_properties(MatPackerCore PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
# The core and library are built without exceptions: failures are reported through return values, so nothing can
# unwind across the C API.
set(MATPACKER_NO_EXCEPTIONS $<IF:$<CXX_COMPILER_ID:MSVC>,/EHs-c-,-fno-exceptions>)
target_compile_options(MatPackerCore PRIVATE ${MATPACKER_NO_EXCEPTIONS})
target_compile_definitions(MatPackerCore PRIVATE $<$<CXX_COMPILER_ID:MSVC>:_HAS_EXCEPTIONS=0>)
target_include_directories(MatPackerCore PUBLIC ${CMAKE_SOURCE_DIR} Src Src/ThirdParty)
target_link_libraries(MatPackerCore PUBLIC ${CMAKE_SOURCE_DIR}/libktx.a)

if(MATPACKER_BUILD_LIBRARY)
    add_library(matpacker SHARED Src/Api/MatPackerApi.cpp)
    target_include_directories(matpacker PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_definitions(matpacker PRIVATE MATPACKER_BUILDING $<$<CXX_COMPILER_ID:MSVC>:_HAS_EXCEPTIONS=0>)
    target_compile_options(matpacker PRIVATE ${MATPACKER_NO_EXCEPTIONS})
    target_link_libraries(matpacker PRIVATE MatPackerCore)
    set_target_properties(matpacker PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        VERSION 1.0.0
        SOVERSION 1
    )

    # Export only the MP* API, so the bundled libktx cannot clash with a host application's own copy.
    if(APPLE)
        target_link_options(matpacker PRIVATE "LINKER:-exported_symbol,_MP*")
    elseif(UNIX)
        target_link_options(matpacker PRIVATE "LINKER:--exclude-libs,ALL")
    endif()

    include(GNUInstallDirs)
    install(TARGETS matpacker
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES Include/MatPacker.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(MATPACKER_BUILD_GUI)
    find_package(wxWidgets 3.3 REQUIRED COMPONENTS core base)
    include(${wxWidgets_USE_FILE})

    add_executable(MatPacker Src/MatPacker.cpp)
    target_link_libraries(MatPacker PRIVATE MatPackerCore ${wxWidgets_LIBRARIES})
endif()
