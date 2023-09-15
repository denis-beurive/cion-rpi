cmake_minimum_required(VERSION 3.0)
project(gpio C)

set(CMAKE_C_STANDARD 11)

add_executable(gpio1 gpio1.c)
target_link_libraries(gpio1 gpiod)

add_executable(gpio2 gpio2.c)
target_link_libraries(gpio2 gpiod)