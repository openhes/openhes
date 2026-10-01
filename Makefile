.PHONY: all clean native-gen native-build arm64-gen arm64-build amd64-gen amd64-build \
        release test format
all: amd64-build

native-build: build/build.ninja
	@echo "Building project..."
	@cmake --build build

build/build.ninja: CMakeLists.txt
	@echo "Generating build files..."
	@cmake -B build -GNinja


arm64-build: build-arm64/build.ninja
	@echo "Cross-building project..."
	@docker run --rm --user "$$(id -u):$$(id -g)" -v "$(PWD):/src" openhes-cross \
	    cmake --build build-arm64

build-arm64/build.ninja: CMakeLists.txt cmake/pi_toolchain_64.cmake
	@echo "Generating cross-build files..."
	@docker run --rm --user "$$(id -u):$$(id -g)" -v "$(PWD):/src" openhes-cross \
	    cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/pi_toolchain_64.cmake \
		-DCMAKE_BUILD_TYPE=Release -GNinja

amd64-build: build-amd64/build.ninja
	@echo "Building project (amd64 container)..."
	@docker run --rm --user "$$(id -u):$$(id -g)" -v "$(PWD):/src" openhes-amd64 \
	    cmake --build build-amd64

build-amd64/build.ninja: CMakeLists.txt
	@echo "Generating build files (amd64 container)..."
	@docker run --rm --user "$$(id -u):$$(id -g)" -v "$(PWD):/src" openhes-amd64 \
	    cmake -B build-amd64 -DOPENHES_BUILD_TESTS=ON -GNinja

# Optimised build (-O3, NDEBUG), as the binary that ships. It gets its own tree
# because CMAKE_BUILD_TYPE belongs to a configure: flipping it in build-amd64
# would rebuild every dependency, and back again on the next "make test".
release: build-release/build.ninja
	@echo "Building release binary (amd64 container)..."
	@docker run --rm --user "$$(id -u):$$(id -g)" -v "$(PWD):/src" openhes-amd64 \
	    cmake --build build-release

build-release/build.ninja: CMakeLists.txt
	@echo "Generating relmd64 build-arm64 build-release (amd64 container)..."
	@docker run --rm --user "$$(id -u):$$(id -g)" -v "$(PWD):/src" openhes-amd64 \
	    cmake -B build-release -DCMAKE_BUILD_TYPE=Release -GNinja

format:
	@find src -type f \( -name "*.c" -o -name "*.h" \) -exec clang-format -i -style=file {} +
	@find tests -type f \( -name "*.c" -o -name "*.h" \) -exec clang-format -i -style=file {} +

# Build (if needed) then run the ctest-registered unit tests in the amd64
# container. Requires OPENHES_BUILD_TESTS=ON (see build-amd64/build.ninja above).
test: amd64-build
	@echo "Running unit tests (amd64 container)..."
	@docker run --rm --user "$$(id -u):$$(id -g)" -v "$(PWD):/src" openhes-amd64 \
	    ctest --test-dir build-amd64 --output-on-failure

clean:
	@echo "Cleaning project..."
	@rm -rf build build-arm64 build-amd64 build-release .temp
