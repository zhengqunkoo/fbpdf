PREFIX = .
CC = cc
CFLAGS = -Wall -O2 -I$(PREFIX)/include
LDFLAGS = -L$(PREFIX)/lib

all: dev-input-mice/mouse.o fbpdf fbdjvu
%.o: %.c doc.h
	$(CC) -c $(CFLAGS) $<
clean:
	-rm -f *.o fbpdf fbdjvu fbpdf2; cd dev-input-mice; make clean

dev-input-mice/mouse.o:
	cd dev-input-mice; make all
# pdf support using mupdf
fbpdf: fbpdf.o mupdf.o draw.o dev-input-mice/mouse.o
	$(CC) -o $@ $^ $(LDFLAGS) -lz -lfreetype -lharfbuzz -ljbig2dec -lopenjp2 -ljpeg -lmupdf -lmupdf-third -lmupdf-pkcs7 -lmupdf-threads -lm -lpthread

# djvu support
fbdjvu: fbpdf.o djvulibre.o draw.o dev-input-mice/mouse.o
	$(CXX) -o $@ $^ $(LDFLAGS) -ldjvulibre -ljpeg -lm -lpthread

# pdf support using poppler
poppler.o: poppler.c
	$(CXX) -c $(CFLAGS) `pkg-config --cflags poppler-cpp` $<
fbpdf2: fbpdf.o poppler.o draw.o
	$(CXX) -o $@ $^ $(LDFLAGS) `pkg-config --libs poppler-cpp`
