cmake_minimum_required(VERSION 3.20)
project(MatPacker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(wxWidgets 3.3 REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})

set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(MatPacker
    Src/MatPacker.cpp
    Src/Ktx2.cpp
    Src/Baker.cpp
    Src/GltfExport.cpp
    Src/Json.cpp
    Src/ImageSource.cpp
    Src/ThirdParty/cgltf.cpp
    Src/ThirdParty/stb_image.cpp
    Src/ThirdParty/stb_image_resize2.cpp
    Src/ThirdParty/stb_image_write.cpp
)
target_include_directories(MatPacker PRIVATE ${CMAKE_SOURCE_DIR} Src Src/ThirdParty)
target_link_libraries(MatPacker PRIVATE ${CMAKE_SOURCE_DIR}/libktx.a ${wxWidgets_LIBRARIES})
