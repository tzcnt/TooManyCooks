from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, cmake_layout
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
    generators = "CMakeDeps", "CMakeToolchain"
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

        # Build Modes - by default (both False), the library is header-only
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
        if self.options.get_safe("windows_dll", False) and not self.options.standalone_compilation:
            raise ConanInvalidConfiguration("windows_dll requires standalone_compilation=True")
        if self.settings.compiler == "msvc":
            raise ConanInvalidConfiguration(
                "TooManyCooks is currently unsupported with MSVC due to a compiler bug: "
                "https://developercommunity.visualstudio.com/t/MSVC-incorrectly-caches-thread_local-var/11041371"
            )
        if self.settings.compiler.get_safe("cppstd"):
            check_min_cppstd(self, self._min_cppstd)
        minimum_version = self._compilers_minimum_version.get(str(self.settings.compiler), False)
        if minimum_version and Version(self.settings.compiler.version) < minimum_version:
            raise ConanInvalidConfiguration(f"{self.ref} requires C++{self._min_cppstd}, which your compiler does not support.")
        
    def export_sources(self):
        copy(self, "LICENSE", src=self.recipe_folder, dst=self.export_sources_folder)
        copy(self, "CMakeLists.txt", src=self.recipe_folder, dst=self.export_sources_folder)
        copy(self, "*.cmake", src=os.path.join(self.recipe_folder, "cmake"), dst=os.path.join(self.export_sources_folder, "cmake"))
        copy(self, "*.cmake.in", src=os.path.join(self.recipe_folder, "cmake"), dst=os.path.join(self.export_sources_folder, "cmake"))
        copy(self, "*.hpp", src=os.path.join(self.recipe_folder, "include"), dst=os.path.join(self.export_sources_folder, "include"))
        copy(self, "*.ipp", src=os.path.join(self.recipe_folder, "include"), dst=os.path.join(self.export_sources_folder, "include"))

    def layout(self):
        cmake_layout(self)

    def package_id(self):
        self.info.settings.clear()

    def build(self):
        cmake = CMake(self)
        cmake.configure(variables={
            "TMC_USE_HWLOC": self.options.with_hwloc,
            "TMC_USE_BOOST_ASIO": self.options.with_asio == "boost",
            "TMC_WORK_ITEM": self.options.work_item,
            "TMC_TRIVIAL_TASK": self.options.trivial_task,
            "TMC_NODISCARD_AWAIT": self.options.nodiscard_await,
            "TMC_PRIORITY_COUNT": "" if self.options.priority_count == None else self.options.priority_count,
            "TMC_MORE_THREADS": self.options.more_threads,
            "TMC_DEBUG_TASK_ALLOC_COUNT": self.options.debug_task_alloc_count,
            "TMC_DEBUG_THREAD_CREATION": self.options.debug_thread_creation,
            "TMC_STANDALONE_COMPILATION": self.options.standalone_compilation,
            "TMC_WINDOWS_DLL": self.options.get_safe("windows_dll", False),
        })
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install(component="TooManyCooks_Development")

    def package_info(self):
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []

        self.cpp_info.set_property("cmake_file_name", "TooManyCooks")
        self.cpp_info.set_property("cmake_target_name", "TooManyCooks::TooManyCooks")

        if self.settings.os in ["Linux", "FreeBSD"]:
            self.cpp_info.system_libs = ["pthread"]
