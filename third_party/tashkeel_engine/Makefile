CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -fPIC -Icpp/include

all: build/libtashkeel.so build/tashkeel_cli

build:
	mkdir -p build

build/libtashkeel.so: cpp/src/diacritizer.cpp cpp/src/c_api.cpp | build
	$(CXX) $(CXXFLAGS) -shared -o $@ cpp/src/diacritizer.cpp cpp/src/c_api.cpp

build/tashkeel_cli: cpp/src/diacritizer.cpp cpp/src/cli.cpp | build
	$(CXX) $(CXXFLAGS) -o $@ cpp/src/diacritizer.cpp cpp/src/cli.cpp

clean:
	rm -rf build

.PHONY: all clean
