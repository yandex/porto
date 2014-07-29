CC=g++
CFLAGS=-c -Wall -g -std=c++11
LDFLAGS=
SOURCES=porto.cpp container.cpp cgroup.cpp
OBJECTS=$(SOURCES:.cpp=.o)
EXECUTABLE=porto

all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

.cpp.o:
	$(CC) $(CFLAGS) $< -o $@

.PHONY: rpc
rpc:
	protoc rpc.proto --cpp_out=.
	g++ container.cpp rpc.cpp rpc.pb.cc -std=c++11 -lprotobuf -o rpc
	echo 'create: { name: "test" }' | ./rpc
	echo 'list: { }' | ./rpc
	echo '' | ./rpc

clean:
	rm -f $(OBJECTS) $(EXECUTABLE)
