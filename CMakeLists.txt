cmake_minimum_required(VERSION 3.16)
project(scrotocol LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)

add_executable(scrotocol
  src/main.cpp
  src/Config.cpp
  src/Capture.cpp
  src/FilterEngine.cpp
  src/CropOverlay.cpp
  src/HistoryStore.cpp
  src/TrayIcon.cpp
  src/MainWindow.cpp
)

target_link_libraries(scrotocol PRIVATE PkgConfig::GTK4 PkgConfig::GIO_UNIX PkgConfig::X11)

install(TARGETS scrotocol RUNTIME DESTINATION bin)
install(FILES data/scrotocol.desktop DESTINATION share/applications)
