REGISTRY     := registry.gitlab.com
DOCKER_IMAGE := registry.gitlab.com/andrewleech/mibsdk:latest
PWD          := $(shell pwd)
TARGET       := libgal_hook.so
BUILD_ID     := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo release)

.PHONY: all hook clean shell

all: hook

hook:
	@echo "==> Compiling native QNX libgal_hook.so inside Docker SDK..."
	@docker run --rm -v $(PWD):/work -w /work $(DOCKER_IMAGE) sh -c "\
		. /etc/qnx/env && \
		arm-unknown-nto-qnx6.5.0eabi-gcc -O2 -Wall -Wextra -Werror -shared -fPIC \
			-I./src -DGAL_HOOK_BUILD='\"$(BUILD_ID)\"' \
			./src/*.c \
			-lsocket \
			-o ./$(TARGET)"
	@echo "==> Build successful: $(TARGET)"

shell:
	@docker run -it --rm -v $(PWD):/work -w /work $(DOCKER_IMAGE) sh -c ". /etc/qnx/env && exec sh"

clean:
	rm -f $(TARGET)
