# Desktop preview of the launcher (needs FreeType: brew install freetype).
CXXFLAGS := -O1 -g -std=c++17 -I. -I../src $(shell pkg-config --cflags freetype2)
LDLIBS := $(shell pkg-config --libs freetype2)

launcher-preview: preview.cpp ../src/launcher.cpp ../src/launcher_gfx.cpp ../src/menu_art.cpp ../src/menu_art.h ../src/launcher_gfx.h ../src/un5_settings.h switch.h
	$(CXX) $(CXXFLAGS) -o $@ preview.cpp ../src/launcher.cpp ../src/launcher_gfx.cpp ../src/menu_art.cpp $(LDLIBS)

clean:
	rm -f launcher-preview
