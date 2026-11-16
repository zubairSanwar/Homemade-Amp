# make        build ./amp (only recompiles the files that changed)
# make run    build and run
# make test   offline DSP tests
# make clean  delete the binary and object files

CXX      = g++
CXXFLAGS = -O2 -std=c++17
LDFLAGS  = -lpthread -ldl -lm

SRC = $(wildcard src/*.cpp src/*/*.cpp)
OBJ = $(SRC:.cpp=.o)

amp: $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

run: amp
	./amp

test:
	bash tests/run_tests.sh

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) amp

.PHONY: run test clean
