PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Default target
EXT_NAME=equation_search
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Use ccache + ninja when available for fast incremental builds
ifneq ("$(wildcard $(shell which ccache))","")
  CCACHE := ccache
endif
ifneq ("$(wildcard $(shell which ninja))","")
  GEN := ninja
endif

include extension-ci-tools/makefiles/duckdb_extension.Makefile