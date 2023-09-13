cmake_minimum_required(VERSION 3.0)
project(gpio_exe C)

set(CMAKE_C_STANDARD 11)

add_executable(gpio_exe main.c)
target_link_libraries(gpio_exe gpiod)
