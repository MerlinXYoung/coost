target("unitest")
    set_kind("binary")
    add_includedirs("../3rd")
    set_default(false)
    add_deps("libco")
    add_files("*.cc")

    add_cxflags("-Wno-sign-compare")

    -- add_cxflags("-Wno-builtin-macro-redefined")
    -- before_build_file(redefine_file_macro)

