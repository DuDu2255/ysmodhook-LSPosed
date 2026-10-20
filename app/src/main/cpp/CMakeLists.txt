cmake_minimum_required(VERSION 3.18)
project(ysmod CXX)

add_library(ysmod SHARED ysmod.cpp)
target_link_libraries(ysmod log android)

