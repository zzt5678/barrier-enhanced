# barrier -- mouse and keyboard sharing utility
# Copyright (C) 2012-2016 Symless Ltd.
# Copyright (C) 2009 Nick Bolton
#
# This package is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# found in the file LICENSE that should have accompanied this file.
#
# This package is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

file(GLOB_RECURSE headers "*.h")
file(GLOB_RECURSE sources "*.cpp")
list(REMOVE_ITEM sources "${CMAKE_CURRENT_SOURCE_DIR}/PolicyTestsMain.cpp")
list(APPEND sources "barrier/PacketStreamFilterTests.cpp")
list(APPEND sources "barrier/TransferArchiveTests.cpp")
list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/barrier/SecureRandomTests.cpp")
list(REMOVE_DUPLICATES sources)

file(GLOB_RECURSE remove_platform "platform/*")
list(REMOVE_ITEM headers ${remove_platform})
list(REMOVE_ITEM sources ${remove_platform})

file(GLOB_RECURSE global_headers "../../test/global/*.h")
file(GLOB_RECURSE global_sources "../../test/global/*.cpp")

list(APPEND headers ${global_headers})
list(APPEND sources ${global_sources})

file(GLOB_RECURSE mock_headers "../../test/mock/*.h")
file(GLOB_RECURSE mock_sources "../../test/mock/*.cpp")

list(APPEND headers ${mock_headers})
list(APPEND sources ${mock_sources})

# platform
if (WIN32)
    file(GLOB platform_sources "platform/MSWindows*.cpp")
    file(GLOB platform_headers "platform/MSWindows*.h")
elseif (APPLE)
    file(GLOB platform_sources "platform/OSX*.cpp")
    file(GLOB platform_headers "platform/OSX*.h")
elseif (UNIX)
    file(GLOB platform_sources "platform/XWindows*.cpp")
    file(GLOB platform_headers "platform/XWindows*.h")
    # This Win32 state machine has no Win32 dependencies so its ordering and
    # stale-result rules remain covered by Linux CI as well.
    list(APPEND platform_sources
        "platform/MSWindowsClipboardSnapshotStateTests.cpp"
        "../../lib/platform/MSWindowsClipboardSnapshotState.cpp")
endif()

list(APPEND sources ${platform_sources})
list(APPEND headers ${platform_headers})

if (UNIX)
    include_directories(
        ../../
        ../../../ext
    )
else()
    include_directories(
        ../../
        ../../../ext
    )
endif()

if (BARRIER_ADD_HEADERS)
    list(APPEND sources ${headers})
endif()

add_executable(unittests ${sources})
target_link_libraries(unittests
    arch base client server common io net platform server synlib mt ipc ${GTEST_LIBRARIES} ${GMOCK_LIBRARIES} ${libs} ${OPENSSL_LIBS})
add_test(NAME unittests COMMAND $<TARGET_FILE:unittests>)

if (WIN32)
    add_executable(policyunittests
        PolicyTestsMain.cpp
        ipc/ElevationPolicyTests.cpp
        ../../lib/ipc/DesktopSwitchPolicy.cpp
        ../../lib/ipc/ElevationPolicy.cpp
    )
    target_link_libraries(policyunittests ${GTEST_LIBRARIES})
    add_test(NAME policyunittests COMMAND $<TARGET_FILE:policyunittests>)

    add_executable(daemonlifecycleunittests
        PolicyTestsMain.cpp
        platform/MSWindowsDaemonLifecycleTests.cpp
    )
    target_link_libraries(daemonlifecycleunittests
        arch base client server common io net platform synlib mt ipc
        ${GTEST_LIBRARIES} ${GMOCK_LIBRARIES} ${libs} ${OPENSSL_LIBS})
    add_test(
        NAME daemonlifecycleunittests
        COMMAND $<TARGET_FILE:daemonlifecycleunittests>)
endif()
