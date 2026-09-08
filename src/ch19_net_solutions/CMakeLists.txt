# 第 19 章：网络练习题答案。复用第 16 章的 Reactor 库。
add_executable(ch19_net_solutions main.cpp solutions.h)
target_include_directories(ch19_net_solutions PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/common
)
target_link_libraries(ch19_net_solutions PRIVATE reactor_lib)
set_target_properties(ch19_net_solutions PROPERTIES FOLDER "Chapters")
