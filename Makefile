CXX ?= clang++
MPICXX ?= mpicxx
PYTHON ?= python3
CPPFLAGS := -Iinclude
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -Wpedantic
LIBOMP ?= $(shell brew --prefix libomp 2>/dev/null || printf /opt/homebrew/opt/libomp)

ifeq ($(shell uname -s),Darwin)
OMP_CXXFLAGS ?= -Xpreprocessor -fopenmp -I$(LIBOMP)/include
OMP_LDFLAGS ?= -L$(LIBOMP)/lib -Wl,-rpath,$(LIBOMP)/lib -lomp
MPI_ENV = OMPI_CXX=$(CXX)
else
OMP_CXXFLAGS ?= -fopenmp
OMP_LDFLAGS ?= -fopenmp
MPI_ENV =
endif

BUILD_DIR := build
PARALLEL_APP := $(BUILD_DIR)/pagerank
TEST := $(BUILD_DIR)/test_pagerank
GENERATOR := $(BUILD_DIR)/generate_web_graph
CONVERTER := $(BUILD_DIR)/convert_snap_to_csr
SAMPLER := $(BUILD_DIR)/sample_induced_csr

COMMON_SOURCES := src/graph.cpp src/pagerank.cpp

.PHONY: all test test-parallel validate clean

all: $(PARALLEL_APP) $(GENERATOR) $(CONVERTER) $(SAMPLER)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(PARALLEL_APP): $(COMMON_SOURCES) src/distributed.cpp src/parallel_main.cpp include/pagerank/distributed.hpp | $(BUILD_DIR)
	$(MPI_ENV) $(MPICXX) $(CPPFLAGS) $(CXXFLAGS) $(OMP_CXXFLAGS) \
		$(COMMON_SOURCES) src/distributed.cpp src/parallel_main.cpp -o $@ $(OMP_LDFLAGS)

$(TEST): $(COMMON_SOURCES) tests/test_pagerank.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(GENERATOR): src/graph.cpp src/generate_web_graph.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(CONVERTER): src/graph.cpp src/convert_snap_to_csr.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(SAMPLER): src/graph.cpp src/sample_induced_csr.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: $(TEST)
	./$(TEST)

test-parallel: $(PARALLEL_APP)
	./$(PARALLEL_APP) --input tests/data/toy_graph.txt --mode serial --verify
	OMP_NUM_THREADS=4 mpirun --bind-to none -np 1 ./$(PARALLEL_APP) --input tests/data/toy_graph.txt --mode hybrid --threads 4 --verify
	OMP_NUM_THREADS=4 mpirun --bind-to none -np 1 ./$(PARALLEL_APP) --input tests/data/toy_graph.txt --mode hybrid --threads 4 --partition in-edges --score-exchange demand --verify
	mpirun --bind-to none -np 4 ./$(PARALLEL_APP) --input tests/data/toy_graph.txt --mode mpi --verify
	mpirun --bind-to none -np 4 ./$(PARALLEL_APP) --input tests/data/toy_graph.txt --mode mpi --partition in-edges --verify
	mpirun --bind-to none -np 4 ./$(PARALLEL_APP) --input tests/data/toy_graph.txt --mode mpi --score-exchange demand --verify
	mpirun --bind-to none -np 4 ./$(PARALLEL_APP) --input tests/data/toy_graph.txt --mode mpi --partition in-edges --score-exchange demand --verify
	mpirun --bind-to none -np 2 ./$(PARALLEL_APP) --input tests/data/toy_graph.txt --mode hybrid --threads 2 --verify
	mpirun --bind-to none -np 2 ./$(PARALLEL_APP) --input tests/data/toy_graph.txt --mode hybrid --threads 2 --partition in-edges --verify
	mpirun --bind-to none -np 2 ./$(PARALLEL_APP) --input tests/data/toy_graph.txt --mode hybrid --threads 2 --partition in-edges --score-exchange demand --verify
	mpirun --bind-to none -np 4 ./$(PARALLEL_APP) --input tests/data/isolated_header_graph.txt --mode mpi --verify
	mpirun --bind-to none -np 4 ./$(PARALLEL_APP) --input tests/data/isolated_header_graph.txt --mode mpi --score-exchange demand --verify
	mpirun --oversubscribe --bind-to none -np 8 ./$(PARALLEL_APP) --input tests/data/isolated_header_graph.txt --mode mpi --score-exchange demand --verify --top-k 0
	./$(PARALLEL_APP) --input tests/data/dangling_graph.txt --mode serial --verify
	mpirun --bind-to none -np 4 ./$(PARALLEL_APP) --input tests/data/dangling_graph.txt --mode mpi --verify
	mpirun --bind-to none -np 4 ./$(PARALLEL_APP) --input tests/data/dangling_graph.txt --mode mpi --partition in-edges --score-exchange demand --verify
	OMP_NUM_THREADS=4 mpirun --bind-to none -np 1 ./$(PARALLEL_APP) --input tests/data/dangling_graph.txt --mode hybrid --threads 4 --score-exchange demand --verify

validate: $(PARALLEL_APP) test
	$(PYTHON) validation/validate_small.py

clean:
	rm -f $(PARALLEL_APP) $(TEST) $(GENERATOR) $(CONVERTER) $(SAMPLER)

