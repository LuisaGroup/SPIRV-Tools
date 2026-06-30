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
    before_build(function(target)
        local script_dir = os.scriptdir()
        local out_dir = path.join(script_dir, "build_generated")
        -- Skip if generated files already exist
        if os.isfile(path.join(out_dir, "build-version.inc")) and
           os.isfile(path.join(out_dir, "generators.inc")) and
           os.isfile(path.join(out_dir, "core_tables_body.inc")) and
           os.isfile(path.join(out_dir, "core_tables_header.inc")) then
            return
        end
        os.mkdir(out_dir)
        -- Find Python
        local python = nil
        local candidates = {"python.exe", "python3.exe", "python3", "python"}
        if find_program then
            for _, name in ipairs(candidates) do
                local prog = find_program(name)
                if prog then python = prog; break end
            end
        end
        if not python then
            local path_str = os.getenv("PATH")
            if path_str then
                for _, pth in ipairs(path.splitenv(path_str)) do
                    for _, name in ipairs(candidates) do
                        local full = path.join(pth, name)
                        if os.isfile(full) then python = full; break end
                    end
                    if python then break end
                end
            end
        end
        if not python then
            raise("Python 3 not found for SPIR-V Tools code generation")
        end
        -- Generate build-version.inc
        local build_version = path.join(out_dir, "build-version.inc")
        local update_script = path.join(script_dir, "utils/update_build_version.py")
        local changes = path.join(script_dir, "CHANGES")
        print("[spirv-tools] Generating build-version.inc...")
        os.execv(python, {update_script, changes, build_version})
        -- Generate generators.inc from the SPIR-V XML registry
        local gen_reg_script = path.join(script_dir, "utils/generate_registry_tables.py")
        local spirv_xml = path.join(script_dir, "..", "spirv-headers/include/spirv/spir-v.xml")
        local generators_inc = path.join(out_dir, "generators.inc")
        print("[spirv-tools] Generating generators.inc...")
        os.execv(python, {gen_reg_script, "--xml=" .. spirv_xml, "--generator-output=" .. generators_inc})
        -- Generate SPIR-V core grammar tables (ggt.py)
        local ggt_script = path.join(script_dir, "utils/ggt.py")
        local grammar_dir = path.join(script_dir, "..", "spirv-headers/include/spirv/unified1")
        local core_grammar = path.join(grammar_dir, "spirv.core.grammar.json")
        local body_output = path.join(out_dir, "core_tables_body.inc")
        local header_output = path.join(out_dir, "core_tables_header.inc")
        local proc_args = {
            ggt_script,
            "--core-tables-body-output=" .. body_output,
            "--core-tables-header-output=" .. header_output,
            "--spirv-core-grammar=" .. core_grammar,
        }
        local extinst_files = {
            {prefix = "",              file = "extinst.glsl.std.450.grammar.json"},
            {prefix = "",              file = "extinst.opencl.std.100.grammar.json"},
            {prefix = "CLDEBUG100_",   file = "extinst.opencl.debuginfo.100.grammar.json"},
            {prefix = "SHDEBUG100_",   file = "extinst.nonsemantic.shader.debuginfo.grammar.json"},
            {prefix = "",              file = "extinst.spv-amd-shader-explicit-vertex-parameter.grammar.json"},
            {prefix = "",              file = "extinst.spv-amd-shader-trinary-minmax.grammar.json"},
            {prefix = "",              file = "extinst.spv-amd-gcn-shader.grammar.json"},
            {prefix = "",              file = "extinst.spv-amd-shader-ballot.grammar.json"},
            {prefix = "",              file = "extinst.debuginfo.grammar.json"},
            {prefix = "",              file = "extinst.nonsemantic.clspvreflection.grammar.json"},
            {prefix = "",              file = "extinst.nonsemantic.vkspreflection.grammar.json"},
            {prefix = "TOSA_",         file = "extinst.tosa.001000.1.grammar.json"},
            {prefix = "",              file = "extinst.arm.motion-engine.100.grammar.json"},
            {prefix = "",              file = "extinst.nonsemantic.graph.debuginfo.grammar.json"},
        }
        for _, ei in ipairs(extinst_files) do
            local full = path.join(grammar_dir, ei.file)
            if os.isfile(full) then
                table.insert(proc_args, "--extinst=" .. ei.prefix .. "," .. full)
            end
        end
        print("[spirv-tools] Generating SPIR-V grammar tables...")
        local result = os.execv(python, proc_args)
        if result ~= 0 then
            raise("ggt.py failed with exit code %d", result)
        end
        print("[spirv-tools] Done: %s, %s", body_output, header_output)
    end)

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
