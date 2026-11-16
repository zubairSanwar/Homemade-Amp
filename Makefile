# make        build ./amp (only recompiles the files that changed)
# make run    build and run with the terminal command loop
# make gui    build and run serving the web UI instead
# make test   offline DSP tests
# make clean  delete the binary and object files

CXX      = g++
CXXFLAGS = -O2 -std=c++17 -MMD -MP
LDFLAGS  = -lpthread -ldl -lm

SRC = $(wildcard src/*.cpp src/*/*.cpp)
OBJ = $(SRC:.cpp=.o)

amp: $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

run: amp
	./amp cli

gui: amp
	./amp gui

test:
	bash tests/run_tests.sh

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) amp

# editing tone.h rebuilds tone.o and amp.o without us listing that by hand
-include $(OBJ:.o=.d)

.PHONY: run gui test clean
