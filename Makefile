CC := gcc
override CFLAGS += -g -Wall -Wshadow -Wcast-align -Wpointer-arith \
	-Wwrite-strings -Wundef -Wredundant-decls -Wextra -Wno-sign-compare \
	-Wformat-security -Wno-pointer-sign -Werror-implicit-function-declaration \
	-Wno-unused-parameter

ifeq ($(D),1)
	override CFLAGS += -DDEBUG_ON
endif

OBJDIR = obj/
PREFIX ?= /usr/local/bin
TARGET = shell

SRC = $(wildcard *.c)
HDR = $(wildcard *.h)
OBJ = $(SRC:%.c=$(OBJDIR)%.o)

ifneq ($(DESTDIR),)
	    INSTALLDIR = $(subst //,/,$(DESTDIR)/$(PREFIX))
	else
	    INSTALLDIR = $(PREFIX)
endif

.PHONY: clean install all new

%::
	@echo $(MAKE)" default, no rule, exiting..."

all : $(TARGET)

install : all
	@mv shell $(INSTALLDIR)
	@echo installed in $(INSTALLDIR)

$(OBJDIR)%.o		:	%.c $(HDR)
	@[[ -d $(OBJDIR) ]] || mkdir $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET)	:	$(OBJ)
	$(CC) $(CFLAGS) $^ -o $@

new : clean all
	@echo "Done"

clean 	:
	@echo -n "Removing [" && ls $(OBJDIR) | xargs echo -n && echo " $(TARGET)]"
	@read -p "Really ? " inp; \
	[[ $$inp = "y" ]] && rm $(OBJDIR)* $(TARGET) || echo "Exiting..."
