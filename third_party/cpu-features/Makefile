SRC=$(NDK_PATH)/sources/android/cpufeatures/cpu-features.c
OBJ=cpu-features.o
LIBCF=libcpu-features.a


$(LIBCF): 
	$(CC) $(CFLAGS) -c $(SRC) -o $(OBJ)
	$(AR) crs $@ $(OBJ)
	cp -a $(LIBCF) $(SYSROOT)/usr/lib

all: $(LIBCF)

clean:
	rm -rf *.o *.a

