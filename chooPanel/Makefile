CXXFLAGS=-Wall -O3 -g -Wextra -Wno-unused-parameter
SOURCES= chooPanel.c tween.c tween_easing.c utils.c
OBJECTS= $(SOURCES:.c=.o)
BINARIES=chooPanel

# Where our library resides. You mostly only need to change the
# RGB_LIB_DISTRIBUTION, this is where the library is checked out.
JSON_INCDIR=/usr/include/json-c
JSON_LIBDIR=/usr/lib
JSON_LIBRARY_NAME=json-c
RGB_INCDIR=/root/workshop/rpi-rgb-led-matrix/include
RGB_LIBDIR=/root/workshop/rpi-rgb-led-matrix/lib
RGB_LIBRARY_NAME=rgbmatrix
RGB_LIBRARY=$(RGB_LIBDIR)/lib$(RGB_LIBRARY_NAME).a

LDFLAGS+=-L$(RGB_LIBDIR) -l$(RGB_LIBRARY_NAME) -L$(JSON_LIBDIR) -l$(JSON_LIBRARY_NAME) -lrt -lm -lpthread -lmosquitto

all : $(BINARIES)

chooPanel: $(OBJECTS) $(RGB_LIBRARY)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)


%.o : %.c
	$(CXX) -I$(RGB_INCDIR) -I$(JSON_INCDIR) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(BINARIES) $(OPTIONAL_OBJECTS) $(OPTIONAL_BINARIES)

FORCE:
.PHONY: FORCE
