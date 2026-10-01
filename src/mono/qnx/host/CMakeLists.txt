# Copyright (c) Xia Zhongyang.
# Licensed under the MIT License.
#
# qnxhost: starts a .NET program with the Mono runtime on QNX Neutrino, in
# place of the dotnet host. It loads the runtime library with dlopen, so it
# needs only the embedding API's headers.
add_executable(qnxhost qnxhost.c)
target_include_directories(qnxhost PRIVATE ${CLR_SRC_NATIVE_DIR}/public)
# System.Native's prototypes, for the P/Invoke override's type.
target_include_directories(qnxhost PRIVATE ${CLR_SRC_NATIVE_DIR} ${CLR_SRC_NATIVE_DIR}/libs/Common
			   ${CLR_SRC_NATIVE_DIR}/libs/System.Native)
install(TARGETS qnxhost RUNTIME DESTINATION bin)
