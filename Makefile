# Compiler
CC = gcc

# Compiler flags: -g for debugging, -Wall for warnings
# $(pkg-config --cflags ...) gets include paths for GLib/GIO
CFLAGS = -g -Wall $(shell pkg-config --cflags glib-2.0 gio-2.0)

# Linker flags: $(pkg-config --libs ...) gets library paths and names for GLib/GIO
LDFLAGS = $(shell pkg-config --libs glib-2.0 gio-2.0)

# Source files
HAL_SRC = src/ble_hal.c
APP_SRC = examples/hal_app.c

# Object files
HAL_OBJ = $(HAL_SRC:.c=.o)
APP_OBJ = $(APP_SRC:.c=.o)

# Executable name
TARGET_APP = hal_app

# Default target: build the sample_app
all: $(TARGET_APP)

# Rule to link the sample_app executable
# It depends on the application's object file and the HAL's object file
$(TARGET_APP): $(APP_OBJ) $(HAL_OBJ)
	$(CC) $(CFLAGS) $^ -o $(TARGET_APP) $(LDFLAGS)

# Rule to compile HAL source file into an object file
# Assumes ble_hal.h is in an 'include' directory, add -Iinclude if so.
# If ble_hal.h is in the same directory as ble_hal.c, -Isrc or no -I is needed for it.
# CFLAGS already includes GLib headers.
$(HAL_OBJ): $(HAL_SRC) include/ble_hal.h
	$(CC) $(CFLAGS) -Iinclude -c $(HAL_SRC) -o $(HAL_OBJ)

# Rule to compile sample_app source file into an object file
# Assumes ble_hal.h is in an 'include' directory.
$(APP_OBJ): $(APP_SRC) include/ble_hal.h
	$(CC) $(CFLAGS) -Iinclude -c $(APP_SRC) -o $(APP_OBJ)

# Clean target: remove object files and the executable
clean:
	rm -f $(HAL_OBJ) $(APP_OBJ) $(TARGET_APP)
