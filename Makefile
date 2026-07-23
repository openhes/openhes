.PHONY: all clean build image
all: build
build:
	@echo "Building project..."
	@docker run --rm -v "$(PWD):/src" openhes-cross
# 	@docker run --rm --platform linux/arm64 -v "${PWD}:/src" openhes-cross

image:
	@echo "Building Docker image..."
	@docker build --no-cache -t openhes-cross -f Dockerfile.crossbuild .
# 	@docker build --platform linux/arm64 --no-cache -t openhes-cross -f Dockerfile.crossbuild .


clean:
	@echo "Cleaning project..."
	@rm -rf build-arm64