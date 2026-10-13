# cmake/CompilerWarnings.cmake
# Attach the project's warning set and optional sanitizers to a target.
# Everything here is PRIVATE: how WE compile is not a usage requirement for consumers.

function(tcpkv_set_warnings target)
    set(gcc_clang_warnings
        -Wall                   # the common set
        -Wextra                 # unused parameters, missing field initialisers, ...
        -Wpedantic              # ISO conformance; warn on GNU extensions
        -Wshadow                # local hides another local or member
        -Wconversion            # implicit value-changing conversions
        -Wsign-conversion       # signed <-> unsigned
        -Wnon-virtual-dtor      # virtual functions but non-virtual destructor
        -Wold-style-cast        # (int)x in C++
        -Wcast-align            # cast increases alignment requirement
        -Wunused                # all -Wunused-* checks
        -Woverloaded-virtual    # derived hides base virtual overload
        -Wnull-dereference
        -Wdouble-promotion      # float silently promoted to double
        -Wformat=2              # printf format checks incl. security
        -Wimplicit-fallthrough) # switch fallthrough without [[fallthrough]]

    set(gcc_only_warnings
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wuseless-cast)

    set(msvc_warnings
        /W4                     # highest practical level
        /permissive-            # standards conformance
        /w14640                 # thread-unsafe static member init
        /w14265                 # class has virtual functions but destructor is not virtual
        /w14062)                # enumerator not handled in switch

    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:${gcc_clang_warnings}>
        $<$<CXX_COMPILER_ID:GNU>:${gcc_only_warnings}>
        $<$<CXX_COMPILER_ID:MSVC>:${msvc_warnings}>)

    if(TCPKV_WERROR)
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
            $<$<CXX_COMPILER_ID:MSVC>:/WX>)
    endif()
endfunction()

function(tcpkv_set_sanitizers target)
    if(TCPKV_SANITIZE)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            # Must appear at BOTH compile and link. Frame pointers and -g make the reports readable.
            target_compile_options(${target} PRIVATE
                -fsanitize=${TCPKV_SANITIZE} -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE
                -fsanitize=${TCPKV_SANITIZE})
            if(TCPKV_SANITIZE MATCHES "undefined")
                # Abort on the first UBSan report instead of printing and continuing.
                target_compile_options(${target} PRIVATE -fno-sanitize-recover=all)
            endif()
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND TCPKV_SANITIZE STREQUAL "address")
            target_compile_options(${target} PRIVATE /fsanitize=address)
        else()
            message(WARNING "TCPKV_SANITIZE='${TCPKV_SANITIZE}' not supported by ${CMAKE_CXX_COMPILER_ID}")
        endif()
    endif()
endfunction()
