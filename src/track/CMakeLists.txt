# heaptrack: bash script to inject/preload
configure_file(heaptrack.sh.cmake
    ${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}/heaptrack @ONLY
)

install(PROGRAMS ${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}/heaptrack
    DESTINATION ${BIN_INSTALL_DIR}
)

if (HEAPTRACK_USE_LIBUNWIND)
    add_library(heaptrack_unwind STATIC trace_libunwind.cpp)
    target_include_directories(heaptrack_unwind PRIVATE ${LIBUNWIND_INCLUDE_DIRS})
    target_link_libraries(heaptrack_unwind PRIVATE ${LIBUNWIND_LIBRARIES})
else()
    add_library(heaptrack_unwind STATIC trace_unwind_tables.cpp)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(LIBUTIL_LIBRARY "util")
endif()

set_property(TARGET heaptrack_unwind PROPERTY POSITION_INDEPENDENT_CODE ON)

# heaptrack_env: runtime environment tests
add_executable(heaptrack_env heaptrack_env.cpp)

install(TARGETS heaptrack_env
    RUNTIME DESTINATION ${LIBEXEC_INSTALL_DIR}
)

set_target_properties(heaptrack_env PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${LIBEXEC_INSTALL_DIR}"
)

# jdictos/prom-metrics-export: live per-file / per-file+line Prometheus
# export via libeen, adjunct to heaptrack's own trace-file output. Linked
# through een_ports_link (not raw find_library) so this resolves against
# the exact same libeen/glib build that the process being watched already
# uses -- a second, independently-linked glib instance in the same process
# crashes very early (confirmed while prototyping this against a real
# een-cmake binary), so this dependency MUST come from the same een-ports
# install tree, not an ad hoc path.
if (HEAPTRACK_ENABLE_PROM_METRICS)
    een_add_library(prom_export TYPE STATIC FILES prom_export.cpp)
    # cpptrace: prom_export.cpp calls cpptrace::get_safe_object_frame()
    # directly and must link it explicitly (previously arrived transitively
    # via libeen-cpp, back when libeen-cpp was linked here for other reasons).
    # libeen-cpp[core]: for een::recursionGuard (een++/util/recursionGuard.h)
    # -- guards recordAlloc/recordFree against re-entering via a nested
    # malloc/free triggered by std::string copy/destruction inside this
    # file's own bookkeeping (e.g. erasing a live-tracking map node runs its
    # value's destructor, which frees a std::string's heap buffer, which
    # re-enters free() -> recordFree() while the map is still mid-erase).
    # libeen-cpp[core] has no glib dependency itself (it's pure C++), so
    # this doesn't reintroduce the double-glib-instance risk that motivated
    # dropping it earlier -- that was actually fixed by matching dynamic
    # glib builds between heaptrack and the target process, not by this.
    een_ports_link(prom_export PRIVATE libeen cpptrace libeen-cpp[core])
    target_compile_definitions(prom_export PUBLIC HEAPTRACK_ENABLE_PROM_METRICS)
    set_property(TARGET prom_export PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

# heaptrack_preload: track a newly started process
een_add_library(heaptrack_preload TYPE SHARED INSTALL
    FILES heaptrack_preload.cpp libheaptrack.cpp
)

target_link_libraries(heaptrack_preload LINK_PRIVATE
    ${CMAKE_DL_LIBS}
    ${CMAKE_THREAD_LIBS_INIT}
    ${LIBUTIL_LIBRARY}
    heaptrack_unwind
    rt
)

if (HEAPTRACK_ENABLE_PROM_METRICS)
    target_link_libraries(heaptrack_preload LINK_PRIVATE prom_export)
    # prom_export.cpp pulls in een++'s RotatingBuffer, which uses
    # std::atomic::wait/notify -- on some toolchains that drags in a
    # GLIBCXX_3.4.35-versioned symbol (std::__cow_string,
    # std::__detail::__wait_impl) that isn't guaranteed to exist in
    # whatever libstdc++.so the TARGET process's environment resolves at
    # runtime. Confirmed live: LD_PRELOAD-ing this .so into a process
    # aborted with "version `GLIBCXX_3.4.35' not found" the moment the
    # runtime linker fell back to an older system libstdc++. This library
    # gets injected into arbitrary host processes via LD_PRELOAD, so
    # ideally it never depends on the host's own C++ runtime version --
    # statically linking ours is what makes that true.
    #
    # But not every build host has libstdc++-static installed (it's a
    # separate devel package from libstdc++-devel), and other hosts run
    # fine without this because their dynamic libstdc++.so already has the
    # newer symbol version -- confirmed live: adding this flag
    # unconditionally broke the build on a a767 with
    # "cannot find -lstdc++: No such file or directory", a machine that
    # never needed it in the first place. So probe for the static archive
    # at configure time and only opt in where it actually exists; hosts
    # without it keep dynamically linking libstdc++, same as before this
    # fix existed.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LINK_OPTIONS -static-libstdc++ -static-libgcc)
    check_cxx_source_compiles("int main(){return 0;}" HEAPTRACK_HAVE_STATIC_LIBSTDCXX)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if (HEAPTRACK_HAVE_STATIC_LIBSTDCXX)
        target_link_options(heaptrack_preload PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endif()

set_target_properties(heaptrack_preload PROPERTIES
    VERSION ${HEAPTRACK_LIB_VERSION}
    SOVERSION ${HEAPTRACK_LIB_SOVERSION}
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${LIB_INSTALL_DIR}/heaptrack"
)

install(TARGETS heaptrack_preload
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}/heaptrack/
)

# heaptrack_inject: track an already running process
add_library(heaptrack_inject MODULE
    heaptrack_inject.cpp
    libheaptrack.cpp
)

target_link_libraries(heaptrack_inject PRIVATE
    ${CMAKE_DL_LIBS}
    ${CMAKE_THREAD_LIBS_INIT}
    ${LIBUTIL_LIBRARY}
    heaptrack_unwind
    rt
    tsl::robin_map
)

set_target_properties(heaptrack_inject PROPERTIES
    VERSION ${HEAPTRACK_LIB_VERSION}
    SOVERSION ${HEAPTRACK_LIB_SOVERSION}
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${LIB_INSTALL_DIR}/heaptrack"
)

install(TARGETS heaptrack_inject
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}/heaptrack/
)

# public API for custom pool allocators or static binaries
install(FILES heaptrack_api.h
    DESTINATION ${CMAKE_INSTALL_PREFIX}/include
)
