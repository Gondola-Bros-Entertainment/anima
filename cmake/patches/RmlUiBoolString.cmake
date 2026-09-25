# RmlUi ba95ffe8: bool conversion always produces exactly one character. Avoid
# the pointer/length assignment that triggers GCC 12's optimized -Wrestrict
# diagnostic through Variant::GetInto<String>. No diagnostics are disabled.
set(header "${RMLUI_SOURCE_DIR}/Include/RmlUi/Core/TypeConverter.inl")
file(READ "${header}" source)
set(original "dest = src ? \"1\" : \"0\";")
set(replacement "dest.assign(1, src ? '1' : '0');")
string(FIND "${source}" "${original}" original_at)
string(FIND "${source}" "${replacement}" patched_at)
if(NOT original_at EQUAL -1)
    string(REPLACE "${original}" "${replacement}" source "${source}")
    file(WRITE "${header}" "${source}")
elseif(patched_at EQUAL -1)
    message(FATAL_ERROR "RmlUi bool converter changed; review the pinned dependency patch")
endif()
