CXXFLAGS := -g3 -Og
LDFLAGS := -lrtmidi

midipush: midipush.cpp
	$(CXX) -o $@ $< $(CXXFLAGS) $(LDFLAGS)
