# Minimal Makefile for a C++17 project laid out as:
#   include/   headers
#   src/*.cpp  sources
# Usage: make -j$(nproc)    make clean    make -n (dry run)
#
# Rules are "target: prerequisites" followed by a TAB-indented recipe.
# Make rebuilds a target when any prerequisite is newer than it (by mtime).

CXX      := g++
# -MMD -MP: write build/foo.d with header dependencies for each object; -MP adds phony targets
# for headers so deleting a header does not break the build.
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Werror -MMD -MP
LDFLAGS  :=
LDLIBS   := -pthread

SRCS := $(wildcard src/*.cpp)
# Rewrite src/x.cpp -> build/x.o for every source.
OBJS := $(SRCS:src/%.cpp=build/%.o)

# Default goal: the first rule in the file.
tcpkv: $(OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)
#   $^ = all prerequisites (the .o files)   $@ = the target (tcpkv)
#   LDLIBS goes AFTER the objects: the linker scans left to right (module 02).

# Pattern rule: how to make any build/%.o from the matching src/%.cpp.
# "| build" is an order-only prerequisite: the directory must exist, but its mtime is ignored.
build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -Iinclude -c $< -o $@
#   $< = first prerequisite (the .cpp)

build:
	mkdir -p build

# Pull in the generated dependency files. The leading '-' means "no error if missing"
# (they do not exist on the first build).
-include $(OBJS:.o=.d)

# .PHONY: these targets are not files. 'make clean' works even if a file named 'clean' exists.
.PHONY: clean
clean:
	rm -rf build tcpkv
