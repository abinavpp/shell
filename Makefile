CC := gcc
override CFLAGS += -g

OBJDIR = obj/
PREFIX ?= /usr/local/bin

SRC = $(wildcard *.c)
HDR = $(wildcard *.h)
OBJ = $(SRC:%.c=$(OBJDIR)%.o)

ifneq ($(DESTDIR),)
	    INSTALLDIR = $(subst //,/,$(DESTDIR)/$(PREFIX))
	else
	    INSTALLDIR = $(PREFIX)
endif

.PHONY: clean install all

%::
	@echo $(MAKE)" default, no rule, exiting..."

all : shell

install : all
	@mv shell $(INSTALLDIR)
	@echo installed in $(INSTALLDIR)

$(OBJDIR)%.o		:	%.c $(HDR)
	@[[ -d $(OBJDIR) ]] || mkdir $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

shell	:	$(OBJ)
	$(CC) $(CFLAGS) $^ -o $@ 

clean 	:
	@echo -n "Removing [" && ls $(OBJDIR) | xargs echo -n && echo "]"
	@read -p "Really ? " inp; \
	[[ $$inp = "y" ]] && rm $(OBJDIR)* || echo "Exiting..."
	
