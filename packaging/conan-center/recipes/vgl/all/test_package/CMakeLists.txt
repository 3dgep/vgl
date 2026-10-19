# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

cmake_minimum_required(VERSION 3.15)
project(test_package LANGUAGES C)

find_package(vgl REQUIRED CONFIG)

add_executable(${PROJECT_NAME} test_package.c)
target_link_libraries(${PROJECT_NAME} PRIVATE vgl::vgl)
if(TARGET vgl::defaults)
    target_link_libraries(${PROJECT_NAME} PRIVATE vgl::defaults)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VGL_TEST_DEFAULTS)
endif()
