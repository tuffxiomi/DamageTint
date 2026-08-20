add_rules("mode.debug", "mode.release")
set_policy("package.requires_lock", true)

package("preloader")
    set_homepage("https://github.com/LiteLDev/preloader-android")
    set_description("Preloader Android")
    add_urls("https://github.com/LiteLDev/preloader-android.git")
    add_versions("main", "main")
    add_deps("cmake")
    on_install("android", function(package)
        import("package.tools.cmake").install(package)
    end)
package_end()

add_requires("preloader")

target("DamageTint")
    set_kind("shared")
    set_languages("c++20")
    set_strip("all")
    add_files("src/DamageTint.cpp")
    add_packages("preloader")

    if is_plat("android") then
        add_cxflags("-fPIC", "-Oz", "-ffunction-sections", "-fdata-sections", "-flto", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-fmerge-all-constants", "-fno-stack-protector", "-fvisibility=hidden")
        add_cxxflags("-fno-rtti", "-fvisibility-inlines-hidden")
        add_shflags("-Wl,--gc-sections", "-Wl,--icf=all", "-flto", "-Wl,--hash-style=gnu", "-Wl,-z,max-page-size=16384")
        add_links("android", "log", "EGL", "GLESv2", "dl")
    end
