CXX := mpic++
CXXFLAGS := -O2 -std=c++17 -fopenmp -Wall
LIBS := -lcrypto

all: hash_cracker sequential

hash_cracker: main.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o hash_cracker main.cpp $(LIBS)

# sequential baseline does not use MPI, but compiling it with mpic++ is fine
# and keeps one toolchain; it simply never calls any MPI function.
sequential: sequential.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o sequential sequential.cpp $(LIBS)

clean:
	rm -f hash_cracker sequential

.PHONY: all clean
