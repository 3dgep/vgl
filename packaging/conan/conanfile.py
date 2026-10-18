# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

"""Conan 2 recipe for vgl (packaging/README.md).

It builds the working tree it sits in: its sources are exported from the repository root.
Zstandard, stb_image and libcurl come from ConanCenter. ConanCenter has no Ogre 14, so the
Ogre backend is left out; it is in the vcpkg port and the source build.
"""

import os
import re

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd, stdcpp_library
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, load, rmdir

required_conan_version = ">=2.0"

# What the build needs from the repository: files at its root, and whole folders. Each folder
# is copied on its own, so that the copy never walks the rest of the tree, such as build/.
SOURCE_FILES = ["CMakeLists.txt", "LICENSE"]
SOURCE_FOLDERS = ["cmake", "include", "src", "backends"]


class VglConan(ConanFile):
    name = "vgl"
    description = ("Lightweight virtual globe library: streams baked elevation and imagery "
                   "packs and draws the globe through OpenGL / OpenGL ES or Direct3D 11, "
                   "behind a C API.")
    license = "MIT"
    url = "https://github.com/3dgep/vgl"
    homepage = "https://github.com/3dgep/vgl"
    topics = ("globe", "terrain", "gis", "rendering", "opengl", "direct3d")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_defaults": [True, False],
        "with_http": [True, False],
        "with_gl": [True, False],
        "with_d3d11": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_defaults": True,
        "with_http": False,
        "with_gl": True,
        "with_d3d11": True,
    }

    def _root(self):
        return os.path.join(self.recipe_folder, "..", "..")

    def set_version(self):
        cmakelists = load(self, os.path.join(self._root(), "CMakeLists.txt"))
        self.version = re.search(r"project\(vgl\s+VERSION\s+([0-9.]+)", cmakelists).group(1)

    def export_sources(self):
        root = self._root()
        for name in SOURCE_FILES:
            copy(self, name, root, self.export_sources_folder, keep_path=False)
        for folder in SOURCE_FOLDERS:
            copy(self, "*", os.path.join(root, folder),
                 os.path.join(self.export_sources_folder, folder))

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC
        else:
            del self.options.with_d3d11

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self, src_folder=".")

    def requirements(self):
        self.requires("zstd/[>=1.5 <2]")
        self.requires("stb/cci.20240531")
        if self.options.with_http:
            self.requires("libcurl/[>=8 <9]")

    def validate(self):
        check_min_cppstd(self, 20)
        if self.options.with_http and not self.options.with_defaults:
            raise ConanInvalidConfiguration("with_http needs with_defaults: the HTTP reader is "
                                            "in the defaults module")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["VGL_BUILD_TESTS"] = False
        tc.cache_variables["VGL_BUILD_SAMPLES"] = False
        tc.cache_variables["VGL_BUILD_BAKER"] = False
        tc.cache_variables["VGL_INSTALL"] = True
        tc.cache_variables["VGL_FETCH_DEPENDENCIES"] = False
        tc.cache_variables["VGL_ZSTD_PROVIDER"] = "SYSTEM"
        tc.cache_variables["VGL_BUILD_DEFAULTS"] = bool(self.options.with_defaults)
        tc.cache_variables["VGL_DEFAULTS_HTTP"] = bool(self.options.with_http)
        tc.cache_variables["VGL_BACKEND_GL"] = bool(self.options.with_gl)
        tc.cache_variables["VGL_BACKEND_D3D11"] = bool(self.options.get_safe("with_d3d11"))
        tc.cache_variables["VGL_BACKEND_OGRE"] = False
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        # Conan describes the package itself (package_info), under the same names.
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "vgl")

        core = self.cpp_info.components["core"]
        core.set_property("cmake_target_name", "vgl::vgl")
        core.set_property("pkg_config_name", "vgl")
        core.libs = ["vgl"]
        core.requires = ["zstd::zstd", "stb::stb"]
        if self.options.shared:
            core.defines = ["VGL_SHARED"]
        if self.settings.os in ("Linux", "FreeBSD"):
            core.system_libs = ["m"]
        # A static vgl is C++, so C programs linking it need the C++ runtime, as the CMake
        # package says too.
        if not self.options.shared and stdcpp_library(self):
            core.system_libs.append(stdcpp_library(self))

        if self.options.with_defaults:
            defaults = self.cpp_info.components["defaults"]
            defaults.set_property("cmake_target_name", "vgl::defaults")
            defaults.set_property("pkg_config_name", "vgl-defaults")
            defaults.libs = ["vgl-defaults"]
            defaults.requires = ["core"]
            if self.options.with_http:
                defaults.requires.append("libcurl::libcurl")
            if self.settings.os in ("Linux", "FreeBSD"):
                defaults.system_libs = ["pthread"]

        if self.options.with_gl:
            gl = self.cpp_info.components["gl"]
            gl.set_property("cmake_target_name", "vgl::gl")
            gl.set_property("pkg_config_name", "vgl-gl")
            gl.libs = ["vgl-gl"]
            gl.requires = ["core"]

        if self.options.get_safe("with_d3d11"):
            d3d11 = self.cpp_info.components["d3d11"]
            d3d11.set_property("cmake_target_name", "vgl::d3d11")
            d3d11.libs = ["vgl-d3d11"]
            d3d11.requires = ["core"]
            d3d11.system_libs = ["d3d11"]
