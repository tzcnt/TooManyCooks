from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.layout import basic_layout
from conan.tools.build import check_min_cppstd
from conan.tools.files import copy
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
        copy(self, "*.hpp", src=os.path.join(self.recipe_folder, "include"), dst=os.path.join(self.export_sources_folder, "include"))
        copy(self, "*.ipp", src=os.path.join(self.recipe_folder, "include"), dst=os.path.join(self.export_sources_folder, "include"))

    def layout(self):
        basic_layout(self)

    def package_id(self):
        self.info.settings.clear()

    def package(self):
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))

        include_dir = os.path.join(self.source_folder, "include")
        copy(self, "*.hpp", src=include_dir, dst=os.path.join(self.package_folder, "include"))
        copy(self, "*.ipp", src=include_dir, dst=os.path.join(self.package_folder, "include"))

    def package_info(self):
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []

        if self.options.with_hwloc:
            self.cpp_info.defines.append("TMC_USE_HWLOC")

        if self.options.with_asio == "boost":
            self.cpp_info.defines.append("TMC_USE_BOOST_ASIO")

        if self.options.standalone_compilation:
            self.cpp_info.defines.append("TMC_STANDALONE_COMPILATION")

        if self.options.get_safe("windows_dll"):
            self.cpp_info.defines.append("TMC_WINDOWS_DLL")

        pc = self.options.priority_count
        if pc != None:
            self.cpp_info.defines.append(f"TMC_PRIORITY_COUNT={pc}")

        if self.options.work_item == "FUNCORO":
            self.cpp_info.defines.append("TMC_WORK_ITEM=FUNCORO")

        if self.options.trivial_task:
            self.cpp_info.defines.append("TMC_TRIVIAL_TASK")

        if self.options.more_threads:
            self.cpp_info.defines.append("TMC_MORE_THREADS")

        if self.options.nodiscard_await:
            self.cpp_info.defines.append("TMC_NODISCARD_AWAIT")

        if self.options.debug_task_alloc_count:
            self.cpp_info.defines.append("TMC_DEBUG_TASK_ALLOC_COUNT")

        if self.options.debug_thread_creation:
            self.cpp_info.defines.append("TMC_DEBUG_THREAD_CREATION")

        if self.settings.os in ["Linux", "FreeBSD"]:
            self.cpp_info.system_libs = ["pthread"]
