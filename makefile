SRCDIR   := src
BUILDDIR := build

INCLUDES := -I./$(SRCDIR)

CFLAGS   := -g -Wall -Wextra -Wno-unused -Wno-unused-parameter -Wno-strict-prototypes -std=c99 -pedantic -MMD -MP $(INCLUDES)
CFLAGS   += -D_GNU_SOURCE
LFLAGS   := 

SRCS     := $(wildcard $(SRCDIR)/*.c) $(wildcard $(SRCDIR)/**/*.c)
OBJS     := $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))
DEPS     := $(OBJS:.o=.d)

TARGET   := $(BUILDDIR)/main

all: $(TARGET)

$(TARGET): $(OBJS) | $(BUILDDIR) $(BUILDDIR)/pm
	$(CC) $^ $(LFLAGS) -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)
	@mkdir -p $(BUILDDIR)/pm

-include $(DEPS)

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean
