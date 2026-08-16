NAME = SLOPECE
DESCRIPTION = "Slope CE Native"
COMPRESSED = YES
COMPRESSED_MODE = zx7

# Prefer runtime speed for the native 3D renderer; -Oz made sense when size
# dominated, but this game is CPU-bound on real calculator hardware.
CFLAGS = -Wall -Wextra -Wno-bitwise-instead-of-logical -O2

include $(shell cedev-config --makefile)
