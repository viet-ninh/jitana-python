cmake_minimum_required(VERSION 3.17)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Python3 development files
find_package(Python3 REQUIRED COMPONENTS Development)

# Add the executable
add_executable(jitana-python main.cpp)

# Include Python headers
target_include_directories(jitana-python PRIVATE ${Python3_INCLUDE_DIRS})

# Link Python libraries
target_link_libraries(jitana-python PRIVATE ${Python3_LIBRARIES})

# Create output directory
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/output/insn)
