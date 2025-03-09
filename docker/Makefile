# zynq-parrot/docker/Makefile.docker
#
# This file defines targets to build, run, and rebuild the zynq-parrot Docker image.
# It assumes that your top-level workspace is one directory up.
#
TOP ?= $(shell git rev-parse --show-toplevel)
# include $(TOP)/Makefile.env
include $(TOP)/Makefile.common

# Define Docker executable
DOCKER ?= docker

# Define Docker platform (default: Ubuntu 20.04)
DOCKER_PLATFORM ?= ubuntu20.04

# Define container name for zynq-parrot
CONTAINER_NAME := zynq-parrot

.PHONY: docker-image docker-run docker-rebuild

docker-image: ## Creates the zynq-parrot Docker image.
	@$(eval USER_ID := $(shell id -u))
	@$(eval GROUP_ID := $(shell id -g))
	@cd $(TOP)/docker && \
		$(DOCKER) build . -f dockerfile.$(DOCKER_PLATFORM) \
		--build-arg USER_ID=$(USER_ID) \
		--build-arg GROUP_ID=$(GROUP_ID) \
		-t $(CONTAINER_NAME)

docker-run: ## Runs the zynq-parrot Docker container mounting the workspace.
	@$(eval USER_ID := $(shell id -u))
	@$(eval GROUP_ID := $(shell id -g))
	@cd $(TOP) && \
		$(DOCKER) run \
			--volume=$(TOP):/home/build/zynq-parrot \
			--volume=/mnt/data/tools/Xilinx:/tools/Xilinx \
			--volume=$(HOME)/.ssh:/home/build/.ssh:ro \
			--env SSH_AUTH_SOCK=$(SSH_AUTH_SOCK) \
			--volume=$(SSH_AUTH_SOCK):$(SSH_AUTH_SOCK) \
			--env DISPLAY=$(DISPLAY) \
			--volume=/tmp/.X11-unix:/tmp/.X11-unix \
			--user $(USER_ID):$(GROUP_ID) \
			-it $(CONTAINER_NAME)

docker-rebuild: ## Rebuilds the Docker image without cache.
	@$(eval USER_ID := $(shell id -u))
	@$(eval GROUP_ID := $(shell id -g))
	@cd $(TOP)/docker && \
		$(DOCKER) build --no-cache . -f dockerfile.$(DOCKER_PLATFORM) \
		--build-arg USER_ID=$(USER_ID) \
		--build-arg GROUP_ID=$(GROUP_ID) \
		-t $(CONTAINER_NAME)
