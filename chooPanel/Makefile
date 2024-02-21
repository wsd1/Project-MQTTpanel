CXXFLAGS=-Wall -O3 -g -Wextra -Wno-unused-parameter
OBJECTS=chooPanel.o
BINARIES=chooPanel

OPTIONAL_OBJECTS=video-viewer.o
OPTIONAL_BINARIES=video-viewer

# Where our library resides. You mostly only need to change the
# RGB_LIB_DISTRIBUTION, this is where the library is checked out.
JSON_INCDIR=/usr/include/json-c
JSON_LIBDIR=/usr/lib
JSON_LIBRARY_NAME=json-c
RGB_INCDIR=/home/dy/rpi-rgb-led-matrix/include
RGB_LIBDIR=/home/dy/rpi-rgb-led-matrix/lib
RGB_LIBRARY_NAME=rgbmatrix
RGB_LIBRARY=$(RGB_LIBDIR)/lib$(RGB_LIBRARY_NAME).a

LDFLAGS+=-L$(RGB_LIBDIR) -l$(RGB_LIBRARY_NAME) -L$(JSON_LIBDIR) -l$(JSON_LIBRARY_NAME) -lrt -lm -lpthread -lmosquitto

all : $(BINARIES)

chooPanel: chooPanel.o $(RGB_LIBRARY)
	$(CXX) $(CXXFLAGS) chooPanel.o -o $@ $(LDFLAGS)


%.o : %.cc
	$(CXX) -I$(RGB_INCDIR) -I$(JSON_INCDIR) $(CXXFLAGS) -c -o $@ $<

chooPanel.o : chooPanel.cc
	$(CXX) -I$(RGB_INCDIR) -I$(JSON_INCDIR) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(BINARIES) $(OPTIONAL_OBJECTS) $(OPTIONAL_BINARIES)

FORCE:
.PHONY: FORCE
