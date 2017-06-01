.PHONY : all clean

all : wmx2obj

clean :
	$(RM) wmx2obj

wmx2obj : wmx2obj.c
	gcc $< -o $@ -Wall -Wextra -Wpedantic
