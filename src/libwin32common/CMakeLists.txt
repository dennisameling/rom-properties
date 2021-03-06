# Windows common code
CMAKE_MINIMUM_REQUIRED(VERSION 3.0)
CMAKE_POLICY(SET CMP0048 NEW)
IF(POLICY CMP0063)
	# CMake 3.3: Enable symbol visibility presets for all
	# target types, including static libraries and executables.
	CMAKE_POLICY(SET CMP0063 NEW)
ENDIF(POLICY CMP0063)
PROJECT(libwin32common LANGUAGES C CXX)

### NOTE: DelayLoadHelper should NOT be compiled here.
# If it is, it won't be linked into the resulting executable,
# since MSVC will trim the object file. Compile it in the
# executable target directly.

# Check for Windows SDK headers.
CHECK_INCLUDE_FILES("shellscalingapi.h" HAVE_SHELLSCALINGAPI_H)

# Check for unordered_set::reserve.
SET(OLD_CMAKE_REQUIRED_INCLUDES ${CMAKE_REQUIRED_INCLUDES})
CHECK_CXX_SOURCE_COMPILES("#include <unordered_set>
int main(void) {
	std::unordered_set<int> testSet;
	testSet.reserve(4);
	return 0;
}" HAVE_UNORDERED_SET_RESERVE)
SET(CMAKE_REQUIRED_INCLUDES ${OLD_CMAKE_REQUIRED_INCLUDES})
UNSET(OLD_CMAKE_REQUIRED_INCLUDES)

# Write the config.h file.
CONFIGURE_FILE("${CMAKE_CURRENT_SOURCE_DIR}/config.libwin32common.h.in" "${CMAKE_CURRENT_BINARY_DIR}/config.libwin32common.h")

# Sources and headers.
SET(libwin32common_SRCS
	DialogBuilder.cpp
	RegKey.cpp
	WinUI.cpp
	w32err.c
	ComBase.cpp
	propsys_xp.c
	userdirs.cpp
	HiDPI.h
	MiniU82T.cpp
	sdk/win_iid.c
	sdk/GUID_fn.c
	)
SET(libwin32common_H
	RpWin32_sdk.h
	DialogBuilder.hpp
	RegKey.hpp
	AutoGetDC.hpp
	WinUI.hpp
	w32err.h
	w32time.h
	ComBase.hpp
	propsys_xp.h
	WTSSessionNotification.hpp
	userdirs.hpp
	HiDPI.c
	MiniU82T.hpp
	sdk/QITab.h
	sdk/IListView.hpp
	sdk/IOwnerDataCallback.hpp
	sdk/tsbase.h
	sdk/windowsx_ts.h
	sdk/commctrl_ts.h
	sdk/GUID_fn.h
	)

######################
# Build the library. #
######################

ADD_LIBRARY(win32common STATIC
	${libwin32common_SRCS}
	${libwin32common_H}
	)
INCLUDE(SetMSVCDebugPath)
SET_MSVC_DEBUG_PATH(win32common)
# Exclude from ALL builds.
SET_TARGET_PROPERTIES(win32common PROPERTIES EXCLUDE_FROM_ALL TRUE)
TARGET_LINK_LIBRARIES(win32common PRIVATE rpthreads)
TARGET_LINK_LIBRARIES(win32common PRIVATE uuid ole32 advapi32 shell32)

TARGET_INCLUDE_DIRECTORIES(win32common
	PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>		# libwin32common
		$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>		# libwin32common
	PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>	# src
		$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/..>	# src
	)

###################
# wmain.c library #
###################
ADD_LIBRARY(wmain STATIC wmain.c)
SET_MSVC_DEBUG_PATH(wmain)
# Exclude from ALL builds.
SET_TARGET_PROPERTIES(wmain PROPERTIES EXCLUDE_FROM_ALL TRUE)
