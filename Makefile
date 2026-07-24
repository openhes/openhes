.PHONY: all clean gen build cross-gen cross-build
all: build

gen:
	@echo "Generating build files..."
	@cmake -B build -GNinja

build:
	@echo "Building project..."
	@cmake --build build

cross-gen:
	@echo "Generating cross-build files..."
	@cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/pi_toolchain_64.cmake -GNinja

cross-build:
	@echo "Cross-building project..."
	@cmake --build build-arm64

clean:
	@echo "Cleaning project..."
	@rm -rf build build-arm64
