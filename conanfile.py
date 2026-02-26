from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.layout import basic_layout
from conan.tools.build import check_min_cppstd
from conan.tools.files import copy, save
from conan.tools.scm import Version
import os

required_conan_version = ">=2.10"

class ToomanycooksConan(ConanFile):
    name = "toomanycooks"
    version = "1.5.0"
    description = "The C++20 coroutine framework with no compromises. Excellent performance, simple syntax, and powerful features."
    license = "BSL-1.0"
    url = "https://github.com/tzcnt/TooManyCooks"
    homepage = "https://github.com/tzcnt/TooManyCooks"
    topics = ("tasking",
              "coroutine",
              "thread-pool",
              "i/o",
              "work-stealing",
              "lockfree",
              "header-only")
    package_type = "header-library"
    settings = "os", "arch", "compiler", "build_type"
    no_copy_source = True
    options = {
        # Dependency Options
        "with_hwloc": [True, False],
        "with_asio": [None, "standalone", "boost"],

        # Configs
        "work_item": ["CORO", "FUNCORO"],
        "trivial_task": [True, False],
        "nodiscard_await": [True, False],
        "priority_count": [None,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16],
        "more_threads": [True, False],
        "debug_task_alloc_count": [True, False],
        "debug_thread_creation": [True, False],

        # Build Options (default is header-only if both are disabled)
        "standalone_compilation": [True, False],
        "windows_dll": [True, False],
    }
    default_options = {
        "with_hwloc": True,
        "with_asio": None,
        "work_item": "CORO",
        "trivial_task": False,
        "nodiscard_await": False,
        "priority_count": None,
        "more_threads": False,
        "debug_task_alloc_count": False,
        "debug_thread_creation": False,
        "standalone_compilation": False,
        "windows_dll": False,
    }
    options_description = {
        "with_hwloc": "https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html#tmc-use-hwloc",
        "with_asio": "allows you to add standalone asio or boost::asio as an optional dependency",
        "work_item": "https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html#tmc-work-item",
        "trivial_task": "https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html#tmc-trivial-task",
        "nodiscard_await": "https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html#tmc-nodiscard-await",
        "priority_count": "https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html#tmc-priority-count",
        "more_threads": "https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html#tmc-more-threads",
        "debug_task_alloc_count": "https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html#tmc-debug-task-alloc-count",
        "debug_thread_creation": "https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html#tmc-debug-thread-creation",
        "standalone_compilation": "https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html#build-modes",
        "windows_dll": "https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html#build-modes",
    }

    def config_options(self):
        if self.settings.os != "Windows":
            del self.options.windows_dll

    @property
    def _min_cppstd(self):
        return 20

    @property
    def _compilers_minimum_version(self):
        return {
            "apple-clang": "17",
            "clang": "17",
            "gcc": "14",
            "msvc": "195",
            "Visual Studio": "18",
        }

    def requirements(self):
        if self.options.with_hwloc:
            self.requires("hwloc/[>=2.4]", transitive_headers=True, transitive_libs=True)
        if self.options.with_asio == "standalone":
            self.requires("asio/[>=1.28 <2]", transitive_headers=True)
        if self.options.with_asio == "boost":
            self.requires(f"boost/[>=1.84]", transitive_headers=True)

    def validate(self):
        if self.settings.compiler.get_safe("cppstd"):
            check_min_cppstd(self, self._min_cppstd)
        minimum_version = self._compilers_minimum_version.get(str(self.settings.compiler), False)
        if minimum_version and Version(self.settings.compiler.version) < minimum_version:
            raise ConanInvalidConfiguration(f"{self.ref} requires C++{self._min_cppstd}, which your compiler does not support.")
        
    def export_sources(self):
        copy(self, "LICENSE", src=self.recipe_folder, dst=self.export_sources_folder)
        copy(self, "CMakeLists.txt", src=self.recipe_folder, dst=self.export_sources_folder)
        copy(self, "*.hpp", src=os.path.join(self.recipe_folder, "include"), dst=os.path.join(self.export_sources_folder, "include"))
        copy(self, "*.ipp", src=os.path.join(self.recipe_folder, "include"), dst=os.path.join(self.export_sources_folder, "include"))

    def layout(self):
        basic_layout(self)

    def package_id(self):
        self.info.settings.clear()

    def package(self):
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "CMakeLists.txt", src=self.source_folder, dst=self.package_folder)

        include_dir = os.path.join(self.source_folder, "include")
        copy(self, "*.hpp", src=include_dir, dst=os.path.join(self.package_folder, "include"))
        copy(self, "*.ipp", src=include_dir, dst=os.path.join(self.package_folder, "include"))

        save(self, os.path.join(self.package_folder, "cmake", "tmc-conan-options.cmake"),
             self._generate_cmake_options())

    def _cmake_bool(self, val):
        return "ON" if val else "OFF"

    def _generate_cmake_options(self):
        lines = [
            f'set(TMC_USE_HWLOC {self._cmake_bool(self.options.with_hwloc)} CACHE BOOL "" FORCE)',
            f'set(TMC_USE_BOOST_ASIO {self._cmake_bool(self.options.with_asio == "boost")} CACHE BOOL "" FORCE)',
            f'set(TMC_WORK_ITEM "{self.options.work_item}" CACHE STRING "" FORCE)',
            f'set(TMC_TRIVIAL_TASK {self._cmake_bool(self.options.trivial_task)} CACHE BOOL "" FORCE)',
            f'set(TMC_NODISCARD_AWAIT {self._cmake_bool(self.options.nodiscard_await)} CACHE BOOL "" FORCE)',
        ]

        pc = self.options.priority_count
        if pc != None:
            lines.append(f'set(TMC_PRIORITY_COUNT "{pc}" CACHE STRING "" FORCE)')
        else:
            lines.append('set(TMC_PRIORITY_COUNT "" CACHE STRING "" FORCE)')

        lines.extend([
            f'set(TMC_MORE_THREADS {self._cmake_bool(self.options.more_threads)} CACHE BOOL "" FORCE)',
            f'set(TMC_DEBUG_TASK_ALLOC_COUNT {self._cmake_bool(self.options.debug_task_alloc_count)} CACHE BOOL "" FORCE)',
            f'set(TMC_DEBUG_THREAD_CREATION {self._cmake_bool(self.options.debug_thread_creation)} CACHE BOOL "" FORCE)',
            f'set(TMC_STANDALONE_COMPILATION {self._cmake_bool(self.options.standalone_compilation)} CACHE BOOL "" FORCE)',
        ])

        wd = self.options.get_safe("windows_dll")
        if wd is not None:
            lines.append(f'set(TMC_WINDOWS_DLL {self._cmake_bool(wd)} CACHE BOOL "" FORCE)')

        # Apply defines to the Conan-generated target based on cache variables.
        # This mirrors the logic in CMakeLists.txt for add_subdirectory consumers.
        lines.append('')
        lines.append('if(TARGET tmc::tmc)')
        lines.append('    if(TMC_USE_HWLOC)')
        lines.append('        target_compile_definitions(tmc::tmc INTERFACE TMC_USE_HWLOC)')
        lines.append('    endif()')
        lines.append('    if(TMC_USE_BOOST_ASIO)')
        lines.append('        target_compile_definitions(tmc::tmc INTERFACE TMC_USE_BOOST_ASIO)')
        lines.append('    endif()')
        lines.append('    if(TMC_WORK_ITEM STREQUAL "FUNCORO")')
        lines.append('        target_compile_definitions(tmc::tmc INTERFACE "TMC_WORK_ITEM=FUNCORO")')
        lines.append('    endif()')
        lines.append('    if(TMC_TRIVIAL_TASK)')
        lines.append('        target_compile_definitions(tmc::tmc INTERFACE TMC_TRIVIAL_TASK)')
        lines.append('    endif()')
        lines.append('    if(TMC_NODISCARD_AWAIT)')
        lines.append('        target_compile_definitions(tmc::tmc INTERFACE TMC_NODISCARD_AWAIT)')
        lines.append('    endif()')
        lines.append('    if(NOT "${TMC_PRIORITY_COUNT}" STREQUAL "")')
        lines.append('        target_compile_definitions(tmc::tmc INTERFACE "TMC_PRIORITY_COUNT=${TMC_PRIORITY_COUNT}")')
        lines.append('    endif()')
        lines.append('    if(TMC_MORE_THREADS)')
        lines.append('        target_compile_definitions(tmc::tmc INTERFACE TMC_MORE_THREADS)')
        lines.append('    endif()')
        lines.append('    if(TMC_DEBUG_TASK_ALLOC_COUNT)')
        lines.append('        target_compile_definitions(tmc::tmc INTERFACE TMC_DEBUG_TASK_ALLOC_COUNT)')
        lines.append('    endif()')
        lines.append('    if(TMC_DEBUG_THREAD_CREATION)')
        lines.append('        target_compile_definitions(tmc::tmc INTERFACE TMC_DEBUG_THREAD_CREATION)')
        lines.append('    endif()')
        lines.append('    if(TMC_STANDALONE_COMPILATION)')
        lines.append('        target_compile_definitions(tmc::tmc INTERFACE TMC_STANDALONE_COMPILATION)')
        lines.append('    endif()')
        lines.append('    if(WIN32 AND TMC_WINDOWS_DLL)')
        lines.append('        target_compile_definitions(tmc::tmc INTERFACE TMC_WINDOWS_DLL)')
        lines.append('    endif()')
        lines.append('endif()')

        return '\n'.join(lines) + '\n'

    def package_info(self):
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []

        self.cpp_info.set_property("cmake_file_name", "tmc")
        self.cpp_info.set_property("cmake_target_name", "tmc::tmc")
        self.cpp_info.set_property("cmake_build_modules", [os.path.join("cmake", "tmc-conan-options.cmake")])

        if self.settings.os in ["Linux", "FreeBSD"]:
            self.cpp_info.system_libs = ["pthread"]
