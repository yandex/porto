CC=g++
CFLAGS=-c -Wall -g -std=c++11
LDFLAGS=
SOURCES=porto.cpp cgroup.cpp
OBJECTS=$(SOURCES:.cpp=.o)
EXECUTABLE=porto

all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

.cpp.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJECTS) $(EXECUTABLE)
