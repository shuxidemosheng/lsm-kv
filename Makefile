CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g
INC      = -Iinclude
BUILD    = build

TESTS = test_skiplist test_sst test_lsm test_wal test_scan

all: $(addprefix $(BUILD)/,$(TESTS)) $(BUILD)/bench

$(BUILD)/test_skiplist: test/test_skiplist.cpp src/skiplist.cpp include/skiplist.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< src/skiplist.cpp

$(BUILD)/test_sst: test/test_sst.cpp src/skiplist.cpp src/sst.cpp include/skiplist.h include/sst.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< src/skiplist.cpp src/sst.cpp

$(BUILD)/test_lsm: test/test_lsm.cpp src/skiplist.cpp src/sst.cpp src/lsm_kv.cpp include/skiplist.h include/sst.h include/bloom.h include/lsm_kv.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< src/skiplist.cpp src/sst.cpp src/lsm_kv.cpp

$(BUILD)/test_wal: test/test_wal.cpp src/skiplist.cpp src/sst.cpp src/lsm_kv.cpp include/skiplist.h include/sst.h include/bloom.h include/lsm_kv.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< src/skiplist.cpp src/sst.cpp src/lsm_kv.cpp

$(BUILD)/test_scan: test/test_scan.cpp src/skiplist.cpp src/sst.cpp src/lsm_kv.cpp include/skiplist.h include/sst.h include/bloom.h include/lsm_kv.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< src/skiplist.cpp src/sst.cpp src/lsm_kv.cpp

$(BUILD)/bench: test/bench.cpp src/skiplist.cpp src/sst.cpp src/lsm_kv.cpp include/skiplist.h include/sst.h include/bloom.h include/lsm_kv.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< src/skiplist.cpp src/sst.cpp src/lsm_kv.cpp

bench: $(BUILD)/bench
	@rm -rf /tmp/lsmkv_bench
	./$(BUILD)/bench

run: all
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
