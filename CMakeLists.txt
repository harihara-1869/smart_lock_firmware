# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Enable LLI Mocking for integration tests
#add_compile_options(-DMOCK_LLI_FOR_TESTING=1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smart_lock_firmware)
