CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g
INC      = -Iinclude
BUILD    = build

TESTS = test_skiplist

all: $(addprefix $(BUILD)/,$(TESTS))

$(BUILD)/%: test/%.cpp src/skiplist.cpp include/skiplist.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< src/skiplist.cpp

run: all
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
