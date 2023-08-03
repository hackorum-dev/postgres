cmake_minimum_required(VERSION 3.20.0)
project(Typedef)

execute_process(COMMAND llvm-config --cmakedir
                OUTPUT_VARIABLE LLVM_CMAKEDIR
                OUTPUT_STRIP_TRAILING_WHITESPACE)

execute_process(COMMAND llvm-config --includedir
                OUTPUT_VARIABLE LLVM_INCLUDEDIR
                OUTPUT_STRIP_TRAILING_WHITESPACE)

message(STATUS ${LLVM_CMAKEDIR})
message(STATUS ${LLVM_INCLUDEDIR})

list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKEDIR})

message(STATUS ${CMAKE_MODULE_PATH})

set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/bin)
set(LLVM_LIBRARY_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib)
set(LLVM_ENABLE_PLUGINS true)

include(HandleLLVMOptions) # important: matches compiler flags to LLVM/Clang build

include(AddLLVM)

add_llvm_library(TypedefCheck MODULE TypedefCheck.cpp PLUGIN_TOOL clang)

target_include_directories(TypedefCheck PUBLIC ${LLVM_INCLUDEDIR})

target_compile_features(TypedefCheck PRIVATE cxx_std_17)
