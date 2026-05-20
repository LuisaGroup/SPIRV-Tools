target("spirv-tools")
    _config_project({
        project_kind = "static"
    })

    -- SPIRV-Headers (for spirv/unified1/*.h)
    add_includedirs(".", "include", {
        public = true
    })
    add_deps('spirv-headers')
    -- Generated .inc files (build-version.inc, core_tables_*.inc, generators.inc, etc.)
    add_includedirs("build_generated")

    -- Windows-specific define expected by SPIRV-Tools sources
    if is_plat("windows") then
        add_defines("SPIRV_WINDOWS")
        add_defines("_CRT_SECURE_NO_WARNINGS", "_SCL_SECURE_NO_WARNINGS")
    end

    -- Core library sources
    add_files(
        "source/util/*.cpp",
        "source/*.cpp",
        "source/util/bit_vector.cpp"
        -- "source/util/parse_number.cpp",
        -- "source/util/string_utils.cpp",
        -- "source/assembly_grammar.cpp",
        -- "source/binary.cpp",
        -- "source/diagnostic.cpp",
        -- "source/disassemble.cpp",
        -- "source/ext_inst.cpp",
        -- "source/extensions.cpp",
        -- "source/libspirv.cpp",
        -- "source/name_mapper.cpp",
        -- "source/opcode.cpp",
        -- "source/operand.cpp",
        -- "source/parsed_operand.cpp",
        -- "source/print.cpp",
        -- "source/software_version.cpp",
        -- "source/spirv_endian.cpp",
        -- "source/spirv_fuzzer_options.cpp",
        -- "source/spirv_optimizer_options.cpp",
        -- "source/spirv_reducer_options.cpp",
        -- "source/spirv_target_env.cpp",
        -- "source/spirv_validator_options.cpp",
        -- "source/table.cpp",
        -- "source/table2.cpp",
        -- "source/text.cpp",
        -- "source/text_handler.cpp",
        -- "source/to_string.cpp"
    )

    -- Validator sources
    add_files("source/val/*.cpp")

    -- Optimizer sources
    add_files("source/opt/*.cpp")

    -- Exclude precompiled-header source stubs
    remove_files("source/pch_source.cpp", "source/opt/pch_source_opt.cpp")

    -- Public headers for installation/IDE visibility
    add_headerfiles("include/spirv-tools/*.h")
    add_headerfiles("include/spirv-tools/*.hpp")
    add_deps('lc-core')

target_end()
