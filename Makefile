SHELL=/bin/bash -O extglob -c
USERID=
CXX=g++
CXXFLAGS= -O3 -Wall -Wextra -std=c++11 -g

SRCDIR = ./src
OBJDIR = ./build
# Add all .cpp files that need to be compiled for your server
SERVER_FILES=server.cpp Packet.h
SERVER_OBJS=$(addprefix $(OBJDIR)/,$(filter %.o,$(SERVER_FILES:.cpp=.o)))

# Add all .cpp files that need to be compiled for your client
CLIENT_FILES=client.cpp Packet.h
CLIENT_OBJS=$(addprefix $(OBJDIR)/,$(filter %.o,$(CLIENT_FILES:.cpp=.o)))

all: server client

debug: CXXFLAGS = -O0 -std=c++11 -Wall -Wextra -g
debug: all

# build/%.o: $(SRCDIR)/%.cpp
# 	$(CXX) -c -o $@ $(CXXFLAGS) $(SRCDIR)/$*.cpp

server: $(addprefix $(SRCDIR)/,$(SERVER_FILES))
	$(CXX) -o $@ $(CXXFLAGS) $(filter-out %.h,$^)

client: $(addprefix $(SRCDIR)/,$(CLIENT_FILES))
	$(CXX) -o $@ $(CXXFLAGS) $(filter-out %.h,$^)

clean:
	# rm -rf $(OBJDIR)
	rm -rf *.tar.gz
	rm -rf *.dSYM/
	rm -f server client

tarball: req-user-id clean
	tar -cvf $(USERID).tar.gz ./!(*.pdf|*.md)

# $(OBJDIR):
# 	mkdir -p $(OBJDIR)

req-user-id:
ifndef USERID
	$(error Run `make tarball USERID=xxx`)
endif

.PHONY: all clean debug tarball req-user-id
