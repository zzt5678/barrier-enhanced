# Barrier -- mouse and keyboard sharing utility
# Copyright (C) 2018 Debauchee Open Source Group
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

if (NOT BARRIER_BUILD_TESTS)
    return()
endif()

if (NOT BARRIER_USE_EXTERNAL_GTEST)
    message(FATAL_ERROR "Tests are enabled but BARRIER_USE_EXTERNAL_GTEST is OFF. "
        "Google Test must be provided externally. Install libgtest-dev / libgmock-dev "
        "or a CMake-native GTest package, then reconfigure with -DBARRIER_USE_EXTERNAL_GTEST=ON.")
endif()

include(FetchContent)

# First, try to find a modern CMake-native GTest installation.
find_package(GTest QUIET)
if(GTest_FOUND)
    # Determine available imported targets.
    if(TARGET GTest::gtest)
        set(GTEST_MAIN_TARGET GTest::gtest)
    elseif(TARGET GTest::GTest)
        set(GTEST_MAIN_TARGET GTest::GTest)
    else()
        message(FATAL_ERROR "GTest package found but no supported gtest imported target (GTest::gtest or GTest::GTest).")
    endif()

    if(TARGET GTest::gmock)
        set(GMOCK_MAIN_TARGET GTest::gmock)
    elseif(TARGET GMock::gmock)
        set(GMOCK_MAIN_TARGET GMock::gmock)
    endif()

    # If we have the main target but no separate gmock target, derive it from GTest.
    if(DEFINED GTEST_MAIN_TARGET AND NOT DEFINED GMOCK_MAIN_TARGET)
        if(TARGET GTest::gmock)
            set(GMOCK_MAIN_TARGET GTest::gmock)
        elseif(TARGET GMock::gmock)
            set(GMOCK_MAIN_TARGET GMock::gmock)
        endif()
    endif()

    # Require at least a gtest target; gmock is optional if not available as a separate target.
    if(NOT DEFINED GTEST_MAIN_TARGET)
        message(FATAL_ERROR "GTest package found but no supported gtest imported target found. "
            "Try installing a newer GTest package or ensure CMake can find it via find_package.")
    endif()

    message(STATUS "Using installed GTest: ${GTEST_MAIN_TARGET}")

    # Derive GTEST_LIBRARIES / GMOCK_LIBRARIES as the link lines downstream expects them.
    set(GTEST_LIBRARIES ${GTEST_MAIN_TARGET})
    set(GMOCK_LIBRARIES ${GMOCK_MAIN_TARGET})

    # Expose a gmock target alias so downstream linking is consistent.
    if(DEFINED GMOCK_MAIN_TARGET AND NOT TARGET gmock)
        add_library(gmock INTERFACE IMPORTED)
        set_target_properties(gmock PROPERTIES INTERFACE_LINK_LIBRARIES "${GMOCK_MAIN_TARGET}")
    endif()
    if(NOT TARGET gtest)
        add_library(gtest INTERFACE IMPORTED)
        set_target_properties(gtest PROPERTIES INTERFACE_LINK_LIBRARIES "${GTEST_MAIN_TARGET}")
    endif()

else()
    # No system GTest found — download and build via FetchContent.
    message(STATUS "GTest not found on system; fetching upstream GoogleTest via FetchContent")

    # Pin to v1.17.0 — matches the system GTest found on this machine
    # (anaconda3 ships GTest 1.17.0). The upstream googletest repo uses
    # "vX.Y.Z" annotated tags (e.g., v1.17.0, v1.14.0).
    set(GTEST_VERSION 1.17.0)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v${GTEST_VERSION}
        GIT_PROGRESS TRUE
    )

    # Prevent FetchContent from capitalising the project name, which can cause
    # find_package compatibility issues with some CMake versions.
    set(gtest_FORCE_SHARED_CRT ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    set(BUILD_GTEST ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(INSTALL_GMOCK OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)

    # After FetchContent_MakeAvailable the imported targets GTest::gtest / GTest::gmock
    # (or GMock::gmock) should be available. Map them to the variables downstream expects.
    if(TARGET GTest::gtest)
        set(GTEST_LIBRARIES GTest::gtest)
    elseif(TARGET GTest::GTest)
        set(GTEST_LIBRARIES GTest::GTest)
    else()
        message(FATAL_ERROR "FetchContent built GTest but no GTest::gtest target found.")
    endif()

    if(TARGET GTest::gmock)
        set(GMOCK_LIBRARIES GTest::gmock)
    elseif(TARGET GMock::gmock)
        set(GMOCK_LIBRARIES GMock::gmock)
    else()
        message(WARNING "FetchContent GTest built but no GMock target found; setting GMOCK_LIBRARIES to empty.")
        set(GMOCK_LIBRARIES "")
    endif()
endif()
