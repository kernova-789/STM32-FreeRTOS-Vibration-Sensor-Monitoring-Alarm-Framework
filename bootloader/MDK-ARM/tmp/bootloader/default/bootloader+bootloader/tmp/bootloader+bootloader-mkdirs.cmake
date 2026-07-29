# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/Users/29951/Documents/Projects/CAN_DISDLAY_NOVA/CAN_DISPLAY/bootloader/MDK-ARM/tmp/bootloader/default/bootloader+bootloader")
  file(MAKE_DIRECTORY "C:/Users/29951/Documents/Projects/CAN_DISDLAY_NOVA/CAN_DISPLAY/bootloader/MDK-ARM/tmp/bootloader/default/bootloader+bootloader")
endif()
file(MAKE_DIRECTORY
  "C:/Users/29951/Documents/Projects/CAN_DISDLAY_NOVA/CAN_DISPLAY/bootloader/MDK-ARM/tmp/bootloader/default/1"
  "C:/Users/29951/Documents/Projects/CAN_DISDLAY_NOVA/CAN_DISPLAY/bootloader/MDK-ARM/tmp/bootloader/default/bootloader+bootloader"
  "C:/Users/29951/Documents/Projects/CAN_DISDLAY_NOVA/CAN_DISPLAY/bootloader/MDK-ARM/tmp/bootloader/default/bootloader+bootloader/tmp"
  "C:/Users/29951/Documents/Projects/CAN_DISDLAY_NOVA/CAN_DISPLAY/bootloader/MDK-ARM/tmp/bootloader/default/bootloader+bootloader/src/bootloader+bootloader-stamp"
  "C:/Users/29951/Documents/Projects/CAN_DISDLAY_NOVA/CAN_DISPLAY/bootloader/MDK-ARM/tmp/bootloader/default/bootloader+bootloader/src"
  "C:/Users/29951/Documents/Projects/CAN_DISDLAY_NOVA/CAN_DISPLAY/bootloader/MDK-ARM/tmp/bootloader/default/bootloader+bootloader/src/bootloader+bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/29951/Documents/Projects/CAN_DISDLAY_NOVA/CAN_DISPLAY/bootloader/MDK-ARM/tmp/bootloader/default/bootloader+bootloader/src/bootloader+bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/29951/Documents/Projects/CAN_DISDLAY_NOVA/CAN_DISPLAY/bootloader/MDK-ARM/tmp/bootloader/default/bootloader+bootloader/src/bootloader+bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
