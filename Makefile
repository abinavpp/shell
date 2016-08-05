CC=gcc

all : pipe.c alias.c util.c
	$(CC)  $^
