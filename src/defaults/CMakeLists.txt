# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Optional defaults: thread pool and filesystem reader, used by the samples and
# available to hosts. Built as a separate vgl_defaults target linking vgl, so
# the core library itself never creates threads (NFR-02). The target is added
# once it has sources.
