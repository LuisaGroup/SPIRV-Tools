-- Native Lua generator for SPIRV-Tools generated tables.
-- Replaces the Python scripts:
--   update_build_version.py
--   generate_registry_tables.py
--   ggt.py (and Table/*.py helpers)

import("core.base.json")
import("core.base.xml")

-- ---------------------------------------------------------------------------
-- Utility helpers
-- ---------------------------------------------------------------------------

local function toint(x)
    if type(x) == "number" then
        return math.floor(x)
    elseif type(x) == "string" then
        local n = tonumber(x)
        if n then return math.floor(n) end
    end
    return x
end

local function escape_json_string(s)
    -- Replicate Python's json.dumps(s).strip('"') for the C string table.
    local out = {}
    for i = 1, #s do
        local c = s:sub(i, i)
        local b = c:byte()
        if c == '"' then
            table.insert(out, '\\"')
        elseif c == '\\' then
            table.insert(out, '\\\\')
        elseif c == '\b' then
            table.insert(out, '\\b')
        elseif c == '\f' then
            table.insert(out, '\\f')
        elseif c == '\n' then
            table.insert(out, '\\n')
        elseif c == '\r' then
            table.insert(out, '\\r')
        elseif c == '\t' then
            table.insert(out, '\\t')
        elseif b < 0x20 then
            table.insert(out, string.format("\\u%04x", b))
        else
            table.insert(out, c)
        end
    end
    return table.concat(out)
end

local function c_str(s)
    return '"' .. escape_json_string(s) .. "\\0\""
end

local function sorted_keys(t)
    local keys = {}
    for k, _ in pairs(t) do
        table.insert(keys, k)
    end
    table.sort(keys)
    return keys
end

local function read_file(filepath)
    filepath = path.unix(filepath)
    if not os.isfile(filepath) then
        return nil
    end
    local f = io.open(filepath, "r")
    if not f then return nil end
    local content = f:read("*all")
    f:close()
    return content
end

local function write_file_if_changed(filepath, content)
    filepath = path.unix(filepath)
    local dir = path.directory(filepath)
    if dir and dir ~= "" then
        os.mkdir(dir)
    end
    local old = read_file(filepath)
    if old == content then
        return
    end
    local f = io.open(filepath, "w")
    if not f then
        raise("failed to open %s for writing", filepath)
    end
    f:write(content)
    f:close()
end

-- ---------------------------------------------------------------------------
-- IndexRange (plain table {first=..., count=...})
-- ---------------------------------------------------------------------------

local function ir(first, count)
    return {first = first, count = count}
end

local function ir_str(r)
    return string.format("IR(%d, %d)", r.first, r.count)
end

local function ir_eq(a, b)
    return a.first == b.first and a.count == b.count
end

-- ---------------------------------------------------------------------------
-- Context
-- ---------------------------------------------------------------------------

local function new_context()
    return {
        string_total_len = 0,
        string_buffer = {},
        strings = {},
        ir_to_string = {},
        range_buffer = {},
        ranges = {}
    }
end

local function ctx_get_string(ctx, ir)
    local key = ir_str(ir)
    local s = ctx.ir_to_string[key]
    if s == nil then
        raise("unregistered index range %s", key)
    end
    return s
end

local function ctx_add_string(ctx, s)
    local existing = ctx.strings[s]
    if existing then
        return existing
    end
    local s_space = #s + 1
    local r = ir(ctx.string_total_len, s_space)
    ctx.strings[s] = r
    ctx.ir_to_string[ir_str(r)] = s
    ctx.string_total_len = ctx.string_total_len + s_space
    table.insert(ctx.string_buffer, s)
    return r
end

local function string_list_key(words)
    local parts = {}
    for _, w in ipairs(words) do
        table.insert(parts, #w)
        table.insert(parts, w)
    end
    return table.concat(parts, "\1")
end

local function ctx_add_string_list(ctx, kind, words)
    if ctx.ranges[kind] == nil then
        ctx.ranges[kind] = {}
        ctx.range_buffer[kind] = {}
    end
    local entry = ctx.ranges[kind]
    local key = string_list_key(words)
    local existing = entry[key]
    if existing then
        return existing
    end
    local new_ranges = {}
    for _, s in ipairs(words) do
        table.insert(new_ranges, ctx_add_string(ctx, s))
    end
    local r = ir(#ctx.range_buffer[kind], #new_ranges)
    for _, rr in ipairs(new_ranges) do
        table.insert(ctx.range_buffer[kind], rr)
    end
    entry[key] = r
    return r
end

-- ---------------------------------------------------------------------------
-- Operand helpers (operate on raw JSON objects)
-- ---------------------------------------------------------------------------

local function op_enumerant(o)
    local result = o.enumerant
    if result == nil then
        raise("operand needs an enumerant string")
    end
    return result
end

local function op_value(o)
    local val = o.value
    if type(val) == "number" then
        return toint(val)
    elseif type(val) == "string" then
        if val:startswith("0x") then
            return tonumber(val)
        else
            return tonumber(val)
        end
    end
    raise("operand needs a value integer or string")
end

local function op_capabilities(o)
    return o.capabilities or {}
end

local function op_extensions(o)
    return o.extensions or {}
end

local function op_aliases(o)
    return o.aliases or {}
end

local function op_parameters(o)
    return o.parameters or {}
end

local function op_version(o)
    return o.version
end

local function op_lastVersion(o)
    return o.lastVersion
end

-- ---------------------------------------------------------------------------
-- Grammar helpers
-- ---------------------------------------------------------------------------

local function convert_min_required_version(version)
    if version == nil then
        return "SPV_SPIRV_VERSION_WORD(1, 0)"
    end
    if version == "None" then
        return "0xffffffffu"
    end
    return "SPV_SPIRV_VERSION_WORD(" .. version:gsub("%.", ",") .. ")"
end

local function convert_max_required_version(version)
    if version == nil then
        return "0xffffffffu"
    end
    return "SPV_SPIRV_VERSION_WORD(" .. version:gsub("%.", ",") .. ")"
end

local function c_bool(b)
    return b and "true" or "false"
end

local function ctype(kind, quantifier)
    if kind == "" then
        raise("operand JSON object missing a 'kind' field")
    end
    if kind == "IdResultType" then
        kind = "TypeId"
    elseif kind == "IdResult" then
        kind = "ResultId"
    elseif kind == "IdMemorySemantics" or kind == "MemorySemantics" then
        kind = "MemorySemanticsId"
    elseif kind == "IdScope" or kind == "Scope" then
        kind = "ScopeId"
    elseif kind == "IdRef" then
        kind = "Id"
    elseif kind == "ImageOperands" then
        kind = "Image"
    elseif kind == "Dim" then
        kind = "Dimensionality"
    elseif kind == "ImageFormat" then
        kind = "SamplerImageFormat"
    elseif kind == "KernelEnqueueFlags" then
        kind = "KernelEnqFlags"
    elseif kind == "LiteralExtInstInteger" then
        kind = "ExtensionInstructionNumber"
    elseif kind == "LiteralSpecConstantOpInteger" then
        kind = "SpecConstantOpNumber"
    elseif kind == "LiteralContextDependentNumber" then
        kind = "TypedLiteralNumber"
    elseif kind == "PairLiteralIntegerIdRef" then
        kind = "LiteralIntegerId"
    elseif kind == "PairIdRefLiteralInteger" then
        kind = "IdLiteralInteger"
    elseif kind == "PairIdRefIdRef" then
        kind = "Id"
    end

    if kind == "FPRoundingMode" then
        kind = "FpRoundingMode"
    elseif kind == "FPFastMathMode" then
        kind = "FpFastMathMode"
    end

    if quantifier == "?" then
        kind = "Optional" .. kind
    elseif quantifier == "*" then
        kind = "Variable" .. kind
    end

    kind = kind:gsub("([a-z])([A-Z])", "%1_%2")
    return "SPV_OPERAND_TYPE_" .. kind:upper()
end

local function convert_operand_kind(obj)
    local kind = obj.kind or ""
    local quantifier = obj.quantifier or ""
    return ctype(kind, quantifier)
end

local function to_safe_identifier(s)
    return "k" .. s:gsub("[^a-zA-Z0-9]", "_")
end

local function prefix_operand_kind_names(prefix, json_dict)
    local old_to_new = {}
    for _, operand_kind in ipairs(json_dict.operand_kinds or {}) do
        local old_name = operand_kind.kind
        local new_name = prefix .. old_name
        operand_kind.kind = new_name
        old_to_new[old_name] = new_name
    end
    for _, instruction in ipairs(json_dict.instructions or {}) do
        for _, operand in ipairs(instruction.operands or {}) do
            local replacement = old_to_new[operand.kind]
            if replacement then
                operand.kind = replacement
            end
        end
    end
end

local EXTENSIONS_FROM_SPIRV_REGISTRY_AND_NOT_FROM_GRAMMARS = {
    "SPV_AMD_gpu_shader_half_float",
    "SPV_AMD_gpu_shader_int16",
    "SPV_KHR_non_semantic_info",
    "SPV_EXT_relaxed_printf_string_address_space",
}

local function get_extension_list(instructions, operand_kinds)
    local things_with_extensions = {}
    for _, item in ipairs(instructions) do
        table.insert(things_with_extensions, item)
    end
    local enumerants = {}
    for _, item in ipairs(operand_kinds) do
        for _, e in ipairs(item.enumerants or {}) do
            table.insert(enumerants, e)
        end
    end
    for _, e in ipairs(enumerants) do
        table.insert(things_with_extensions, e)
    end

    local extension_set = {}
    for _, item in ipairs(things_with_extensions) do
        for _, ext in ipairs(item.extensions or {}) do
            extension_set[ext] = true
        end
    end

    for _, ext in ipairs(EXTENSIONS_FROM_SPIRV_REGISTRY_AND_NOT_FROM_GRAMMARS) do
        if extension_set[ext] then
            raise("Extension %s is already in a grammar file", ext)
        end
        extension_set[ext] = true
    end

    extension_set["SPV_VALIDATOR_ignore_type_decl_unique"] = true

    local extensions = {}
    for ext, _ in pairs(extension_set) do
        table.insert(extensions, ext)
    end
    table.sort(extensions)
    return extensions
end

-- ---------------------------------------------------------------------------
-- ExtInst
-- ---------------------------------------------------------------------------

local function new_extinst(spec)
    local prefix, file = spec:match("^([^,]*),(.*)$")
    if not prefix then
        raise("Invalid prefix and path: %s", spec)
    end
    local self = {prefix = prefix, file = file}
    self.name = file:match(".*extinst%.(.*)%.grammar%.json$")
    if not self.name then
        raise("Invalid grammar file name: %s", file)
    end
    self.name = self.name:gsub("-", "_"):gsub("%.", "_")
    self.enum_name = "SPV_EXT_INST_TYPE_" .. self.name:upper()
    if self.enum_name == "SPV_EXT_INST_TYPE_OPENCL_STD_100" then
        self.enum_name = "SPV_EXT_INST_TYPE_OPENCL_STD"
    elseif self.enum_name == "SPV_EXT_INST_TYPE_NONSEMANTIC_SHADER_DEBUGINFO" then
        self.enum_name = "SPV_EXT_INST_TYPE_NONSEMANTIC_SHADER_DEBUGINFO_100"
    end
    self.grammar = json.loadfile(self.file)
    if self.prefix and #self.prefix > 0 then
        prefix_operand_kind_names(self.prefix, self.grammar)
    end
    return self
end

-- ---------------------------------------------------------------------------
-- Grammar
-- ---------------------------------------------------------------------------

local function should_emit_operand_kind(operand_kind_json)
    local category = operand_kind_json.category
    return category == "ValueEnum" or category == "BitEnum"
end

local function new_grammar(extensions, operand_kinds, printing_classes)
    local self = {
        context = new_context(),
        extensions = {},
        operand_kinds = {},
        printing_classes = {},
        header_ignore_decls = {},
        header_decls = {},
        body_decls = {},
        operand_kinds_needing_optional_variant = {
            "ImageOperands",
            "AccessQualifier",
            "MemoryAccess",
            "PackedVectorFormat",
            "CooperativeMatrixOperands",
            "MatrixMultiplyAccumulateOperands",
            "RawAccessChainOperands",
            "FPEncoding",
            "TensorOperands",
            "Capability",
        },
    }

    for _, e in ipairs(extensions) do table.insert(self.extensions, e) end
    table.sort(self.extensions)

    for _, ok in ipairs(operand_kinds) do table.insert(self.operand_kinds, ok) end
    table.sort(self.operand_kinds, function(a, b)
        return convert_operand_kind(a) < convert_operand_kind(b)
    end)

    for _, pc in ipairs(printing_classes) do
        table.insert(self.printing_classes, to_safe_identifier(pc))
    end
    table.sort(self.printing_classes)

    if #self.operand_kinds == 0 then
        raise("operand_kinds should be a non-empty list")
    end
    if #self.extensions == 0 then
        raise("extensions should be a non-empty list")
    end

    table.insert(self.header_ignore_decls, [[
struct IndexRange {
  uint32_t first = 0; // index of the first element in the range
  uint32_t count = 0; // number of elements in the range
};
constexpr inline IndexRange IR(uint32_t first, uint32_t count) {
  return {first, count};
}
]])

    return self
end

local function grammar_compute_printing_class_decls(self)
    local parts = {"enum class PrintingClass : uint32_t {"}
    for _, x in ipairs(self.printing_classes) do
        table.insert(parts, "  " .. x .. ",")
    end
    table.insert(parts, "};\n")
    for _, p in ipairs(parts) do
        table.insert(self.header_decls, p)
    end
end

local function grammar_compute_extension_decls(self)
    local parts = {"enum Extension : uint32_t {"}
    for _, e in ipairs(self.extensions) do
        table.insert(parts, "  " .. to_safe_identifier(e) .. ",")
    end
    table.insert(parts, "};\n")
    for _, p in ipairs(parts) do
        table.insert(self.header_decls, p)
    end

    parts = {}
    table.insert(parts, "// Returns the name of an extension, as an index into kStrings")
    table.insert(parts, "IndexRange ExtensionToIndexRange(Extension extension) {\n  switch(extension) {")
    for _, e in ipairs(self.extensions) do
        table.insert(parts, " case Extension::k" .. e .. ": return " .. ir_str(ctx_add_string(self.context, e)) .. ";")
    end
    table.insert(parts, " default: break;")
    table.insert(parts, "  }\n  return {};\n}\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    parts = {}
    table.insert(parts, "// Extension names and values, ordered by name")
    table.insert(parts, "// The fields in order are:")
    table.insert(parts, "// name, indexing into kStrings")
    table.insert(parts, "// enum value")
    table.insert(parts, "static const std::array<NameValue," .. #self.extensions .. "> kExtensionNames{{")
    for _, e in ipairs(self.extensions) do
        table.insert(parts, " {" .. ir_str(ctx_add_string(self.context, e)) .. ", static_cast<uint32_t>(" .. to_safe_identifier(e) .. ")},")
    end
    table.insert(parts, "}};\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end
end

local function grammar_compute_operand_tables(self)
    table.insert(self.header_ignore_decls, [[
struct NameIndex {
  // Location of the null-terminated name in the global string table.
  IndexRange name;
  // Index of this name's entry in in the associated by-value table.
  uint32_t index;
};
struct NameValue {
  // Location of the null-terminated name in the global string table.
  IndexRange name;
  // Enum value in the binary format.
  uint32_t value;
};
// Describes a SPIR-V operand.
struct OperandDesc {
  uint32_t value;
  IndexRange operands_range; // Indexes kOperandSpans
  IndexRange name_range; // Indexes kStrings
  IndexRange aliases_range; // Indexes kAliasSpans
  IndexRange capabilities_range;  // Indexes kCapabilitySpans
  // A set of extensions that enable this feature. If empty then this operand
  // value is in core and its availability is subject to minVersion. The
  // assembler, binary parser, and disassembler ignore this rule, so you can
  // freely process invalid modules.
  IndexRange extensions_range;  // Indexes kExtensionSpans
  // Minimal core SPIR-V version required for this feature, if without
  // extensions. ~0u means reserved for future use. ~0u and non-empty
  // extension lists means only available in extensions.
  uint32_t minVersion;
  uint32_t lastVersion;
  utils::Span<spv_operand_type_t> operands() const;
  utils::Span<char> name() const;
  utils::Span<IndexRange> aliases() const;
  utils::Span<spv::Capability> capabilities() const;
  utils::Span<spvtools::Extension> extensions() const;
  OperandDesc(const OperandDesc&) = delete;
  OperandDesc(OperandDesc&&) = delete;
};
]])

    local operands_by_value = {}
    local operands_by_value_by_kind = {}
    local index_by_kind_and_value = {}
    local index = 0
    for _, operand_kind_json in ipairs(self.operand_kinds) do
        local kind_key = convert_operand_kind(operand_kind_json)
        if should_emit_operand_kind(operand_kind_json) then
            local operand_descs = {}
            local operands = {}
            for _, o in ipairs(operand_kind_json.enumerants or {}) do
                table.insert(operands, o)
            end
            table.sort(operands, function(a, b) return op_value(a) < op_value(b) end)
            for _, o in ipairs(operands) do
                local suboperands = {}
                for _, p in ipairs(op_parameters(o)) do
                    table.insert(suboperands, convert_operand_kind(p))
                end
                local desc = {
                    op_value(o),
                    ir_str(ctx_add_string_list(self.context, "operand", suboperands)),
                    ir_str(ctx_add_string(self.context, op_enumerant(o))) .. "/* " .. op_enumerant(o) .. " */",
                    ir_str(ctx_add_string_list(self.context, "alias", op_aliases(o))),
                    ir_str(ctx_add_string_list(self.context, "capability", op_capabilities(o))),
                    ir_str(ctx_add_string_list(self.context, "extension", op_extensions(o))),
                    convert_min_required_version(op_version(o)),
                    convert_max_required_version(op_lastVersion(o)),
                }
                table.insert(operand_descs, "{" .. table.concat(desc, ",") .. "}, // " .. kind_key)
                index_by_kind_and_value[kind_key .. "\0" .. tostring(op_value(o))] = index
                index = index + 1
            end
            operands_by_value_by_kind[kind_key] = ir(#operands_by_value, #operand_descs)
            for _, d in ipairs(operand_descs) do
                table.insert(operands_by_value, d)
            end
        end
    end

    local parts = {}
    table.insert(parts, "// Operand descriptions, ordered by (operand kind, operand enum value).")
    table.insert(parts, "// The fields in order are:")
    table.insert(parts, "// enum value")
    table.insert(parts, "// operands, an IndexRange into kOperandSpans")
    table.insert(parts, "// name, a character-counting IndexRange into kStrings")
    table.insert(parts, "// aliases, an IndexRange into kAliasSpans")
    table.insert(parts, "// capabilities, an IndexRange into kCapabilitySpans")
    table.insert(parts, "// extensions, as an IndexRange into kExtensionSpans")
    table.insert(parts, "// version, first version of SPIR-V that has it")
    table.insert(parts, "// lastVersion, last version of SPIR-V that has it")
    table.insert(parts, "static const std::array<OperandDesc, " .. #operands_by_value .. "> kOperandsByValue{{")
    for _, x in ipairs(operands_by_value) do
        table.insert(parts, "  " .. x)
    end
    table.insert(parts, "}};\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    parts = {}
    table.insert(parts, "// Maps an operand kind to possible operands for that kind.")
    table.insert(parts, "// The result is an IndexRange into kOperandsByValue, and the operands")
    table.insert(parts, "// are sorted by value within that span.")
    table.insert(parts, "// An optional variant of a kind maps to the details for the corresponding")
    table.insert(parts, "// concrete operand kind.")
    table.insert(parts, "IndexRange OperandByValueRangeForKind(spv_operand_type_t type) {\n  switch(type) {")
    for _, kind_key in ipairs(sorted_keys(operands_by_value_by_kind)) do
        local r = operands_by_value_by_kind[kind_key]
        table.insert(parts, " case " .. kind_key .. ": return " .. ir_str(r) .. ";")
    end
    for _, kind in ipairs(self.operand_kinds_needing_optional_variant) do
        local non_optional_kind = ctype(kind, "")
        if operands_by_value_by_kind[non_optional_kind] then
            table.insert(parts, " case " .. ctype(kind, "?") .. ": return " .. ir_str(operands_by_value_by_kind[non_optional_kind]) .. ";")
        else
            raise("error: unknown operand type %s, from JSON grammar operand '%s': consider updating spv_operand_type_t in spirv-tools/libspirv.h",
                non_optional_kind, kind)
        end
    end
    table.insert(parts, " default: break;")
    table.insert(parts, "  }\n  return IR(0,0);\n}\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    local operand_names = {}
    local name_range_for_kind = {}
    for _, operand_kind_json in ipairs(self.operand_kinds) do
        local kind_key = convert_operand_kind(operand_kind_json)
        if should_emit_operand_kind(operand_kind_json) then
            local operands = {}
            for _, o in ipairs(operand_kind_json.enumerants or {}) do
                table.insert(operands, o)
            end
            local tuples = {}
            for _, o in ipairs(operands) do
                table.insert(tuples, {op_enumerant(o), op_value(o), kind_key})
                for _, a in ipairs(op_aliases(o)) do
                    table.insert(tuples, {a, op_value(o), kind_key})
                end
            end
            table.sort(tuples, function(a, b) return a[1] < b[1] end)
            local ir_tuples = {}
            for _, t in ipairs(tuples) do
                table.insert(ir_tuples, {ctx_add_string(self.context, t[1]), t[2], t[3]})
            end
            name_range_for_kind[kind_key] = ir(#operand_names, #ir_tuples)
            for _, t in ipairs(ir_tuples) do
                table.insert(operand_names, t)
            end
        end
    end

    local operand_name_strings = {}
    for i = 1, #operand_names do
        local r, value, kind_key = operand_names[i][1], operand_names[i][2], operand_names[i][3]
        local idx = index_by_kind_and_value[kind_key .. "\0" .. tostring(value)]
                        table.insert(operand_name_strings, "{" .. ir_str(r) .. ", " .. idx .. "}, // " .. (i - 1) .. " " .. ctx_get_string(self.context, r) .. " in " .. kind_key)
    end

    parts = {}
    table.insert(parts, "// Operand names and index into kOperandsByValue, ordered by (operand kind, name)")
    table.insert(parts, "// The fields in order are:")
    table.insert(parts, "// name, either the primary name or an alias, indexing into kStrings")
    table.insert(parts, "// index into the kOperandsByValue array")
    table.insert(parts, "static const std::array<NameIndex, " .. #operand_name_strings .. "> kOperandNames{{")
    for _, x in ipairs(operand_name_strings) do
        table.insert(parts, "  " .. x)
    end
    table.insert(parts, "}};\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    parts = {}
    table.insert(parts, "// Maps an operand kind to possible names for operands of that kind.")
    table.insert(parts, "// The result is an IndexRange into kOperandNames, and the names")
    table.insert(parts, "// are sorted by name within that span.")
    table.insert(parts, "// An optional variant of a kind maps to the details for the corresponding")
    table.insert(parts, "// concrete operand kind.")
    table.insert(parts, "IndexRange OperandNameRangeForKind(spv_operand_type_t type) {\n  switch(type) {")
    for _, kind_key in ipairs(sorted_keys(name_range_for_kind)) do
        local r = name_range_for_kind[kind_key]
        table.insert(parts, " case " .. kind_key .. ": return " .. ir_str(r) .. ";")
    end
    for _, kind in ipairs(self.operand_kinds_needing_optional_variant) do
        local non_optional_kind = ctype(kind, "")
        if name_range_for_kind[non_optional_kind] then
            table.insert(parts, " case " .. ctype(kind, "?") .. ": return " .. ir_str(name_range_for_kind[non_optional_kind]) .. ";")
        end
    end
    table.insert(parts, " default: break;")
    table.insert(parts, "  }\n  return IR(0,0);\n}\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end
end

local function grammar_compute_instruction_tables(self, insts)
    table.insert(self.header_ignore_decls, [[
// Describes an Instruction
struct InstructionDesc {
  const spv::Op value;
  const bool hasResult;
  const bool hasType;
  const IndexRange operands_range; // Indexes kOperandSpans
  const IndexRange name_range; // Indexes kStrings
  const IndexRange aliases_range; // Indexes kAliasSpans
  const IndexRange capabilities_range;  // Indexes kCapbilitySpans
  // A set of extensions that enable this feature. If empty then this operand
  // value is in core and its availability is subject to minVersion. The
  // assembler, binary parser, and disassembler ignore this rule, so you can
  // freely process invalid modules.
  const IndexRange extensions_range; // Indexes kExtensionSpans
  // Minimal core SPIR-V version required for this feature, if without
  // extensions. ~0u means reserved for future use. ~0u and non-empty
  // extension lists means only available in extensions.
  uint32_t minVersion;
  uint32_t lastVersion;
  PrintingClass printingClass; // Section of SPIR-V spec. e.g. kComposite, kImage
  utils::Span<spv_operand_type_t> operands() const;
  utils::Span<char> name() const;
  utils::Span<IndexRange> aliases() const;
  utils::Span<spv::Capability> capabilities() const;
  utils::Span<spvtools::Extension> extensions() const;
  OperandDesc(const OperandDesc&) = delete;
  OperandDesc(OperandDesc&&) = delete;
};
]])

    local lines = {}
    local index_by_opcode = {}
    local sorted_insts = {}
    for _, inst in ipairs(insts) do
        table.insert(sorted_insts, inst)
    end
    table.sort(sorted_insts, function(a, b) return toint(a.opcode) < toint(b.opcode) end)

    for _, inst in ipairs(sorted_insts) do
        local opname = inst.opname
        local operand_kinds = {}
        for _, o in ipairs(inst.operands or {}) do
            table.insert(operand_kinds, convert_operand_kind(o))
        end
        if opname == "OpExtInst" and operand_kinds[#operand_kinds] == "SPV_OPERAND_TYPE_VARIABLE_ID" then
            table.remove(operand_kinds)
        end

        local hasResult = false
        local hasType = false
        for _, ok in ipairs(operand_kinds) do
            if ok == "SPV_OPERAND_TYPE_RESULT_ID" then hasResult = true end
            if ok == "SPV_OPERAND_TYPE_TYPE_ID" then hasType = true end
        end

        local aliases = {}
        for _, name in ipairs(inst.aliases or {}) do
            table.insert(aliases, name:sub(3))
        end

        local parts = {
            "spv::Op::" .. opname,
            c_bool(hasResult),
            c_bool(hasType),
            ir_str(ctx_add_string_list(self.context, "operand", operand_kinds)),
            ir_str(ctx_add_string(self.context, opname:sub(3))),
            ir_str(ctx_add_string_list(self.context, "alias", aliases)),
            ir_str(ctx_add_string_list(self.context, "capability", inst.capabilities or {})),
            ir_str(ctx_add_string_list(self.context, "extension", inst.extensions or {})),
            convert_min_required_version(inst.version),
            convert_max_required_version(inst.lastVersion),
            "PrintingClass::" .. to_safe_identifier(inst.class or "@exclude")
        }

        index_by_opcode[toint(inst.opcode)] = #lines
                    table.insert(lines, "{" .. table.concat(parts, ", ") .. "},")
    end

    local parts = {}
    table.insert(parts, "// Instruction descriptions, ordered by opcode.")
    table.insert(parts, "// The fields in order are:")
    table.insert(parts, "// opcode")
    table.insert(parts, "// a boolean indicating if the instruction produces a result ID")
    table.insert(parts, "// a boolean indicating if the instruction result ID has a type")
    table.insert(parts, "// operands, an IndexRange into kOperandSpans")
    table.insert(parts, "// opcode name (without the 'Op' prefix), a character-counting IndexRange into kStrings")
    table.insert(parts, "// aliases, an IndexRange into kAliasSpans")
    table.insert(parts, "// capabilities, an IndexRange into kCapabilitySpans")
    table.insert(parts, "// extensions, as an IndexRange into kExtensionSpans")
    table.insert(parts, "// version, first version of SPIR-V that has it")
    table.insert(parts, "// lastVersion, last version of SPIR-V that has it")
    table.insert(parts, "static const std::array<InstructionDesc, " .. #lines .. "> kInstructionDesc{{")
    for _, l in ipairs(lines) do
        table.insert(parts, "  " .. l)
    end
    table.insert(parts, "}};\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    local opcode_name_entries = {}
    local name_value_pairs = {}
    for _, i in ipairs(insts) do
        table.insert(name_value_pairs, {i.opname:sub(3), toint(i.opcode)})
        for _, a in ipairs(i.aliases or {}) do
            table.insert(name_value_pairs, {a:sub(3), toint(i.opcode)})
        end
    end
    table.sort(name_value_pairs, function(a, b) return a[1] < b[1] end)
    local inst_name_strings = {}
    for i = 1, #name_value_pairs do
        local name, value = name_value_pairs[i][1], name_value_pairs[i][2]
        local r = ctx_add_string(self.context, name)
        local idx = index_by_opcode[value]
                    table.insert(inst_name_strings, "{" .. ir_str(r) .. ", " .. idx .. "}, // " .. (i - 1) .. " " .. name)
    end

    parts = {}
    table.insert(parts, "// Opcode strings (without the 'Op' prefix) and opcode values, ordered by name.")
    table.insert(parts, "// The fields in order are:")
    table.insert(parts, "// name, either the primary name or an alias, indexing into kStrings")
    table.insert(parts, "// index into kInstructionDesc")
    table.insert(parts, "static const std::array<NameIndex, " .. #inst_name_strings .. "> kInstructionNames{{")
    for _, x in ipairs(inst_name_strings) do
        table.insert(parts, "  " .. x)
    end
    table.insert(parts, "}};\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end
end

local function grammar_compute_extended_instructions(self, extinsts)
    local by_value = {}
    local by_value_by_kind = {}
    local index_by_kind_and_opcode = {}
    local index = 0
    for _, e in ipairs(extinsts) do
        local insts_in_set = {}
        local sorted_insts = {}
        for _, inst in ipairs(e.grammar.instructions or {}) do
            table.insert(sorted_insts, inst)
        end
        table.sort(sorted_insts, function(a, b) return toint(a.opcode) < toint(b.opcode) end)
        for _, inst in ipairs(sorted_insts) do
            local operands = {}
            for _, o in ipairs(inst.operands or {}) do
                table.insert(operands, convert_operand_kind(o))
            end
            local inst_parts = {
                toint(inst.opcode),
                ir_str(ctx_add_string_list(self.context, "operand", operands)),
                ir_str(ctx_add_string(self.context, inst.opname)),
                ir_str(ctx_add_string_list(self.context, "capability", inst.capabilities or {})),
            }
            table.insert(insts_in_set, "    {" .. table.concat(inst_parts, ",") .. "}, // " .. inst.opname .. " in " .. e.name)
            index_by_kind_and_opcode[e.enum_name .. "\0" .. tostring(toint(inst.opcode))] = index
            index = index + 1
        end
        by_value_by_kind[e.enum_name] = ir(#by_value, #insts_in_set)
        for _, item in ipairs(insts_in_set) do
            table.insert(by_value, item)
        end
    end

    local parts = {}
    table.insert(parts, "// Extended instruction descriptions, ordered by (extinst enum, opcode value).")
    table.insert(parts, "// The fields in order are:")
    table.insert(parts, "//   enum value")
    table.insert(parts, "//   operands, an IndexRange into kOperandSpans")
    table.insert(parts, "//   name, a character-counting IndexRange into kStrings")
    table.insert(parts, "//   capabilities, an IndexRange into kCapabilitySpans")
    table.insert(parts, "static const std::array<ExtInstDesc, " .. #by_value .. "> kExtInstByValue{{")
    for _, x in ipairs(by_value) do
        table.insert(parts, x)
    end
    table.insert(parts, "}};\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    parts = {}
    table.insert(parts, "// Maps an extended instruction enum to possible names for operands of that kind.")
    table.insert(parts, "// The result is an IndexRange into kOperandNames, and the names")
    table.insert(parts, "// are sorted by name within that span.")
    table.insert(parts, "// An optional variant of a kind maps to the details for the corresponding")
    table.insert(parts, "// concrete operand kind.")
    table.insert(parts, "IndexRange ExtInstByValueRangeForKind(spv_ext_inst_type_t type) {\n  switch(type) {")
    for _, name in ipairs(sorted_keys(by_value_by_kind)) do
        local r = by_value_by_kind[name]
        table.insert(parts, " case " .. name .. ": return " .. ir_str(r) .. ";")
    end
    table.insert(parts, "    default: break;")
    table.insert(parts, "  }\n  return IR(0,0);\n}\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    parts = {}
    local by_name = {}
    local by_name_by_kind = {}
    for _, e in ipairs(extinsts) do
        local insts_by_name = {}
        for _, inst in ipairs(e.grammar.instructions or {}) do
            table.insert(insts_by_name, inst)
        end
        table.sort(insts_by_name, function(a, b) return a.opname < b.opname end)
        local insts_in_set = {}
        for _, inst in ipairs(insts_by_name) do
            local idx = index_by_kind_and_opcode[e.enum_name .. "\0" .. tostring(toint(inst.opcode))]
            table.insert(insts_in_set, "    {" .. ir_str(ctx_add_string(self.context, inst.opname)) .. ", " .. idx .. "}, // " .. inst.opname .. " in " .. e.name)
        end
        by_name_by_kind[e.enum_name] = ir(#by_name, #insts_in_set)
        for _, item in ipairs(insts_in_set) do
            table.insert(by_name, item)
        end
    end

    table.insert(parts, "// Extended instruction opcode names sorted by extended instruction kind, then opcode name.")
    table.insert(parts, "// The fields in order are:")
    table.insert(parts, "//   name")
    table.insert(parts, "//   index into kExtInstByValue")
    table.insert(parts, "static const std::array<NameIndex, " .. #by_name .. "> kExtInstNames{{")
    for _, x in ipairs(by_name) do
        table.insert(parts, x)
    end
    table.insert(parts, "}};\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    parts = {}
    table.insert(parts, "// Maps an extended instruction kind to possible names for instructions of that kind.")
    table.insert(parts, "// The result is an IndexRange into kExtInstNames, and the names")
    table.insert(parts, "// are sorted by name within that span.")
    table.insert(parts, "IndexRange ExtInstNameRangeForKind(spv_ext_inst_type_t type) {\n  switch(type) {")
    for _, name in ipairs(sorted_keys(by_name_by_kind)) do
        local r = by_name_by_kind[name]
        table.insert(parts, " case " .. name .. ": return " .. ir_str(r) .. ";")
    end
    table.insert(parts, "    default: break;")
    table.insert(parts, "  }\n  return IR(0,0);\n}\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end
end

local function grammar_compute_leaf_tables(self)
    local parts = {}
    table.insert(parts, "// Array of characters, referenced by IndexRanges elsewhere.")
    table.insert(parts, "// Each IndexRange denotes a string.")
    table.insert(parts, "static const char kStrings[] =")
    for _, s in ipairs(self.context.string_buffer) do
        table.insert(parts, "  " .. c_str(s) .. " // " .. ir_str(self.context.strings[s]))
    end
    table.insert(parts, ";\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    parts = {}
    table.insert(parts, "// Array of IndexRanges, where each represents a string by referencing")
    table.insert(parts, "// the kStrings table.")
    table.insert(parts, "// This array contains all sequences of alias strings used in the grammar.")
    table.insert(parts, "// This table is referenced by an IndexRange elsewhere, i.e. by the 'aliases'")
    table.insert(parts, "// field of an instruction or operand description.")
    table.insert(parts, "static const IndexRange kAliasSpans[] = {")
    local ranges = self.context.range_buffer["alias"] or {}
    for i = 1, #ranges do
        local r = ranges[i]
        table.insert(parts, "  " .. ir_str(r) .. ", // " .. (i - 1) .. " " .. ctx_get_string(self.context, r))
    end
    table.insert(parts, "};\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    parts = {}
    table.insert(parts, "// Array of capabilities, referenced by IndexRanges elsewhere.")
    table.insert(parts, "// Contains all sequences of capabilities used in the grammar.")
    table.insert(parts, "static const spv::Capability kCapabilitySpans[] = {")
    local capability_ranges = self.context.range_buffer["capability"] or {}
    for i = 1, #capability_ranges do
        local r = capability_ranges[i]
        local cap = ctx_get_string(self.context, r)
        table.insert(parts, "  spv::Capability::" .. cap .. ", // " .. (i - 1))
    end
    table.insert(parts, "};\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    parts = {}
    table.insert(parts, "// Array of extensions, referenced by IndexRanges elsewhere.")
    table.insert(parts, "// Contains all sequences of extensions used in the grammar.")
    table.insert(parts, "static const spvtools::Extension kExtensionSpans[] = {")
    ranges = self.context.range_buffer["extension"] or {}
    for i = 1, #ranges do
        local r = ranges[i]
        local name = ctx_get_string(self.context, r)
        table.insert(parts, "  spvtools::Extension::k" .. name .. ", // " .. (i - 1))
    end
    table.insert(parts, "};\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end

    parts = {}
    table.insert(parts, "// Array of operand types, referenced by IndexRanges elsewhere.")
    table.insert(parts, "// Contains all sequences of operand types used in the grammar.")
    table.insert(parts, "static const spv_operand_type_t kOperandSpans[] = {")
    ranges = self.context.range_buffer["operand"] or {}
    for i = 1, #ranges do
        local r = ranges[i]
        local name = ctx_get_string(self.context, r)
        table.insert(parts, "  " .. name .. ", // " .. (i - 1))
    end
    table.insert(parts, "};\n")
    for _, p in ipairs(parts) do
        table.insert(self.body_decls, p)
    end
end

-- ---------------------------------------------------------------------------
-- Top-level generation functions
-- ---------------------------------------------------------------------------

local function generate_build_version(script_dir, out_dir)
    local changes_file = path.join(script_dir, "CHANGES")
    local output_file = path.join(out_dir, "build-version.inc")

    local version = nil
    local f = io.open(path.unix(changes_file), "r")
    if not f then
        raise("could not open CHANGES file: %s", changes_file)
    end
    for line in f:lines() do
        local v = line:match("^(v%d+%.%d+%-dev) %d%d%d%d%-%d%d%-%d%d%s*$")
        if not v then
            v = line:match("^(v%d+%.%d+) %d%d%d%d%-%d%d%-%d%d%s*$")
        end
        if v then
            version = v
            break
        end
    end
    f:close()
    if not version then
        raise("Could not deduce latest release version from %s.", changes_file)
    end

    local description = os.getenv("FORCED_BUILD_VERSION_DESCRIPTION")
    if not description or description == "" then
        local repo_path = path.directory(changes_file)
        local git_describe = nil
        local result = try {function() return os.iorunv("git", {"rev-parse", "--show-toplevel"}, {curdir = repo_path}) end}
        if result and result:trim() ~= "" then
            result = try {function() return os.iorunv("git", {"describe", "--tags", "--match=v*", "--long"}, {curdir = repo_path}) end}
            if not result or result:trim() == "" then
                result = try {function() return os.iorunv("git", {"rev-parse", "HEAD"}, {curdir = repo_path}) end}
            end
            if result then
                git_describe = result:trim()
            end
        end
        if git_describe and git_describe ~= "" then
            description = git_describe
        else
            local timestamp = os.getenv("SOURCE_DATE_EPOCH")
            if timestamp and timestamp ~= "" then
                timestamp = tonumber(timestamp)
            else
                timestamp = os.time()
            end
            description = "unknown hash, " .. os.date("!%Y-%m-%dT%H:%M:%S", timestamp) .. "+00:00"
        end
    end

    local content = '"' .. version .. '", "SPIRV-Tools ' .. version .. " " .. description .. '"\n'
    write_file_if_changed(output_file, content)
end

local function generate_generators(script_dir, out_dir)
    local spirv_xml = path.join(script_dir, "..", "spirv-headers/include/spirv/spir-v.xml")
    local output_file = path.join(out_dir, "generators.inc")
    local doc = xml.loadfile(spirv_xml)

    local lines = {}
    for _, ids in ipairs(doc.children or {}) do
        if ids.name == "ids" and ids.attrs and ids.attrs.type == "vendor" then
            for _, an_id in ipairs(ids.children or {}) do
                if an_id.name == "id" and an_id.attrs then
                    local value = an_id.attrs.value
                    local vendor = an_id.attrs.vendor
                    local tool = an_id.attrs.tool or ""
                    local vendor_tool = vendor
                    if tool ~= "" then
                        vendor_tool = vendor .. " " .. tool
                    end
                    table.insert(lines, "{" .. value .. ', "' .. vendor .. '", "' .. tool .. '", "' .. vendor_tool .. '"},')
                end
            end
        end
    end
    local content = table.concat(lines, "\n") .. "\n"
    write_file_if_changed(output_file, content)
end

local function generate_core_tables(script_dir, out_dir)
    local grammar_dir = path.join(script_dir, "..", "spirv-headers/include/spirv/unified1")
    local core_grammar = path.join(grammar_dir, "spirv.core.grammar.json")
    local body_output = path.join(out_dir, "core_tables_body.inc")
    local header_output = path.join(out_dir, "core_tables_header.inc")

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

    local extinst_specs = {}
    for _, ei in ipairs(extinst_files) do
        local full = path.join(grammar_dir, ei.file)
        if os.isfile(full) then
            table.insert(extinst_specs, ei.prefix .. "," .. full)
        end
    end

    if #extinst_specs < 1 then
        raise("missing extinst grammar files")
    end

    local extinsts = {}
    for _, spec in ipairs(extinst_specs) do
        table.insert(extinsts, new_extinst(spec))
    end
    table.sort(extinsts, function(a, b) return a.name < b.name end)

    local core_grammar_obj = json.loadfile(core_grammar)
    local printing_class = {}
    for _, e in ipairs(core_grammar_obj.instruction_printing_class or {}) do
        table.insert(printing_class, e.tag)
    end

    local instructions = {}
    for _, x in ipairs(core_grammar_obj.instructions or {}) do
        table.insert(instructions, x)
    end
    local operand_kinds = {}
    for _, x in ipairs(core_grammar_obj.operand_kinds or {}) do
        table.insert(operand_kinds, x)
    end
    for _, e in ipairs(extinsts) do
        for _, x in ipairs(e.grammar.instructions or {}) do
            table.insert(instructions, x)
        end
        for _, x in ipairs(e.grammar.operand_kinds or {}) do
            table.insert(operand_kinds, x)
        end
    end

    local extensions = get_extension_list(instructions, operand_kinds)

    local g = new_grammar(extensions, operand_kinds, printing_class)
    grammar_compute_printing_class_decls(g)
    grammar_compute_extension_decls(g)
    grammar_compute_operand_tables(g)
    grammar_compute_instruction_tables(g, core_grammar_obj.instructions)
    grammar_compute_extended_instructions(g, extinsts)
    grammar_compute_leaf_tables(g)

    write_file_if_changed(body_output, table.concat(g.body_decls, "\n"))
    write_file_if_changed(header_output, table.concat(g.header_decls, "\n"))
end

function generate(script_dir, out_dir)
    os.mkdir(out_dir)
    generate_build_version(script_dir, out_dir)
    generate_generators(script_dir, out_dir)
    generate_core_tables(script_dir, out_dir)
end
