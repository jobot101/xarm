cmake_minimum_required(VERSION 3.16)
# shown as "running <version>" on the web Firmware panel: the commit this build came from
# (falls back to a build timestamp, so every build still reads differently)
execute_process(COMMAND git rev-parse --short HEAD OUTPUT_VARIABLE PROJECT_VER
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PROJECT_VER)
  string(TIMESTAMP PROJECT_VER "%Y%m%d-%H%M%S")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xarm-voice)
