cmake_minimum_required(VERSION 2.6)

add_executable(jitana-python
    main.cpp
)
target_link_libraries(jitana-python
    python
)

find_package(Python3 REQUIRED COMPONENTS Development)

target_include_directories(jitana-python PRIVATE ${Python3_INCLUDE_DIRS})

target_link_libraries(jitana-python PRIVATE ${Python3_LIBRARIES})

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/output/insn)
