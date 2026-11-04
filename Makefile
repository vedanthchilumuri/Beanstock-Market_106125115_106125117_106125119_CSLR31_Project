CXX ?= g++
CPPFLAGS := -Iinclude
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

COMMON_SRCS := src/health_companion.cpp src/food_database.cpp src/food_database_client.cpp

ifeq ($(OS),Windows_NT)
  CONSOLE_TARGET := a.exe
  GUI_TARGET := gui.exe
  RESET_TARGET := reset_data.exe
  WINHTTP_LIBS := -lwinhttp
  GUI_LIBS := -lwinhttp -luser32 -lgdi32 -lcomctl32
  ALL_TARGETS := $(CONSOLE_TARGET) $(GUI_TARGET) $(RESET_TARGET)
else
  CONSOLE_TARGET := health_companion
  GUI_TARGET :=
  RESET_TARGET := reset_data
  CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
  CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
  ifeq ($(strip $(CURL_LIBS)),)
    CURL_LIBS := -lcurl
  endif
  LIBS := $(CURL_CFLAGS) $(CURL_LIBS) -pthread
  ALL_TARGETS := $(CONSOLE_TARGET) $(RESET_TARGET)
endif

.PHONY: all clean help console gui reset

all: $(ALL_TARGETS)

console: $(CONSOLE_TARGET)

gui: $(GUI_TARGET)

reset: $(RESET_TARGET)

$(CONSOLE_TARGET): src/main.cpp $(COMMON_SRCS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/main.cpp $(COMMON_SRCS) -o $@ $(WINHTTP_LIBS) $(LIBS)

$(GUI_TARGET): src/windows_gui.cpp $(COMMON_SRCS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/windows_gui.cpp $(COMMON_SRCS) -o $@ -mwindows $(GUI_LIBS)

$(RESET_TARGET): src/reset_data.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/reset_data.cpp -o $@

clean:
	rm -f $(ALL_TARGETS)

help:
	@echo "Targets:"
	@echo "  make            Build the default targets for this OS"
	@echo "  make console    Build the terminal app"
	@echo "  make gui        Build the Windows GUI app (Windows only)"
	@echo "  make reset      Build the reset utility"
	@echo "  make clean      Remove built binaries"
