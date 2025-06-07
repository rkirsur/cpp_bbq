# Compiler
CXX = clang++

# Compiler flags
CXXFLAGS = -Wall -Wno-uninitialized

BINARIES = main

all: ${BINARIES}

test: ${BINARIES}
	./main

main: main.cpp bbq.h
	$(CXX) $(CXXFLAGS) -o main main.cpp

clean:
	rm -f main main.o