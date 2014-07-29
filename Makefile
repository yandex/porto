CC=g++
CFLAGS=-c -Wall -g -std=c++11
LDFLAGS=-lprotobuf
SOURCES=portod.cpp rpc.cpp rpc.pb.cc container.cpp cgroup.cpp
OBJECTS=$(SOURCES:.cpp=.o)
EXECUTABLE=portod
PROTOBUF=rpc.pb.h rpc.pb.cc

all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS) $(PROTOBUF)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

.cpp.o:
	$(CC) $(CFLAGS) $< -o $@

$(PROTOBUF):
	protoc rpc.proto --cpp_out=.

clean:
	rm -f $(OBJECTS) $(EXECUTABLE) $(PROTOBUF)
