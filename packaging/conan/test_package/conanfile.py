# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

"""Checks a vgl Conan package with the repository's consumer project (tests/install): the C
API and the C++ wrappers, with the targets the package's options give."""

import os

from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class VglTestConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"

    def requirements(self):
        self.requires(self.tested_reference_str)

    def layout(self):
        cmake_layout(self)

    def generate(self):
        options = self.dependencies["vgl"].options
        targets = []
        if options.with_defaults:
            targets.append("vgl::defaults")
        if options.with_gl:
            targets.append("vgl::gl")
        if options.get_safe("with_d3d11"):
            targets.append("vgl::d3d11")
        tc = CMakeToolchain(self)
        tc.cache_variables["VGL_EXPECTED_TARGETS"] = ";".join(targets)
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure(
            build_script_folder=os.path.join(self.recipe_folder, "..", "..", "..", "tests",
                                             "install"))
        cmake.build()

    def test(self):
        if can_run(self):
            self.run(f"ctest --test-dir \"{self.build_folder}\" -C {self.settings.build_type} "
                     "--output-on-failure", env="conanrun")
