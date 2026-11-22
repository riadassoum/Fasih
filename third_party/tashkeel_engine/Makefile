CXX = g++
UNAME_M := $(shell uname -m)
ARCH_FLAGS := $(if $(filter x86_64 amd64,$(UNAME_M)),-mpopcnt,)
CXXFLAGS = -std=c++17 -O3 $(ARCH_FLAGS) -Wall -Wextra -fPIC -fvisibility=hidden -Icpp/include

all: linux/libbarq.so linux/barq

linux:
	mkdir -p linux

linux/libbarq.so: cpp/src/diacritizer.cpp cpp/src/c_api.cpp | linux
	$(CXX) $(CXXFLAGS) -shared -o $@ cpp/src/diacritizer.cpp cpp/src/c_api.cpp

linux/barq: cpp/src/diacritizer.cpp cpp/src/cli.cpp | linux
	$(CXX) $(CXXFLAGS) -o $@ cpp/src/diacritizer.cpp cpp/src/cli.cpp

clean:
	rm -rf linux/libbarq.so linux/barq build

.PHONY: all clean
