local root_dir = path.getabsolute(".")

workspace "zx-spectrum"
    location "build/msvc"
    architecture "x86_64"
    configurations { "Debug", "Release" }
    startproject "zx-spectrum"

project "zx-spectrum"
    kind "WindowedApp"
    entrypoint "mainCRTStartup"
    language "C"
    cdialect "C11"
    system "windows"
    systemversion "latest"
    characterset "MBCS"
    staticruntime "On"
    warnings "Extra"
    disablewarnings { "4324", "4702" }
    linkoptions { "/STACK:16777216" }

    targetname "zx-spectrum"
    targetdir (path.join(root_dir, "redist"))
    objdir (path.join(root_dir, "build/msvc/obj/%{cfg.buildcfg}"))
    debugdir (path.join(root_dir, "redist"))

    files {
        "src/main.c",
        "src/file_util.c",
        "src/spectrum.c",
        "src/next_machine.c",
        "src/next_dma.c",
        "src/next_copper.c",
        "src/next_sprite.c",
        "src/next_sd.c",
        "src/next_rtc.c",
        "src/next_wifi.c",
        "src/szx_inflate.c",
        "src/zip.c",
        "src/disk.c",
        "src/microdrive.c",
        "src/tape.c",
        "src/*.h",
        "src/resources.rc",
        "src/assets/**"
    }

    includedirs {
        "src",
        "third_party/chips",
        "third_party/scintilla/include"
    }

    defines {
        "_CRT_SECURE_NO_WARNINGS"
    }

    links {
        "gdi32",
        "comdlg32",
        "comctl32",
        "shell32",
        "user32",
        "winmm",
        "ws2_32"
    }

    postbuildcommands {
        "{COPYFILE} \"" .. path.join(root_dir, "third_party/scintilla/bin/Scintilla.dll") .. "\" \"" .. path.join(root_dir, "redist/Scintilla.dll") .. "\"",
        "{MKDIR} \"" .. path.join(root_dir, "redist/roms") .. "\"",
        "{COPYDIR} \"" .. path.join(root_dir, "src/roms") .. "\" \"" .. path.join(root_dir, "redist/roms") .. "\""
    }

    filter "configurations:Debug"
        defines { "_DEBUG" }
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        defines { "NDEBUG" }
        runtime "Release"
        optimize "Speed"

    filter {}
