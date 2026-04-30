#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: build_extra_tools.sh <manifest> <bundle_obj>" >&2
    exit 1
fi

if [ -z "${CC:-}" ]; then
    echo "CC is not set" >&2
    exit 1
fi

manifest_path=$1
bundle_obj_path=$2
bundle_dir=$(dirname "$bundle_obj_path")
tool_obj_dir="$bundle_dir/extra_tools"
registry_c_path="$bundle_dir/extra_tools_registry.c"
registry_o_path="$bundle_dir/extra_tools_registry.o"
tmp_dir="$bundle_dir/.tmp"
header_path="config/tool_registry.h"
script_path=$0

mkdir -p "$bundle_dir" "$tool_obj_dir" "$tmp_dir"

trim_field() {
    printf '%s' "$1" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'
}

escape_c_string() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

sanitize_symbol() {
    printf '%s' "$1" | sed 's/[^A-Za-z0-9_]/_/g'
}

write_if_changed() {
    tmp_path=$1
    target_path=$2

    if [ -f "$target_path" ] && cmp -s "$tmp_path" "$target_path"; then
        rm -f "$tmp_path"
        return
    fi

    mv "$tmp_path" "$target_path"
}

compile_c_object() {
    output_path=$1
    source_path=$2
    extra_flags=$3
    rename_main=$4

    cmd="$CC $CFLAGS -I. $extra_flags -Dmain=$rename_main -c \"$source_path\" -o \"$output_path\""
    eval "$cmd"
}

compile_registry_object() {
    output_path=$1
    source_path=$2

    cmd="$CC $CFLAGS -I. -c \"$source_path\" -o \"$output_path\""
    eval "$cmd"
}

if [ ! -f "$manifest_path" ]; then
    printf "" > "$manifest_path"
fi

registry_tmp="$tmp_dir/extra_tools_registry.c.tmp"
{
    printf '#include "config/tool_registry.h"\n\n'
} > "$registry_tmp"

extra_count=0
extra_objects=""

while IFS='|' read -r raw_name raw_source raw_description raw_argv0 raw_cflags; do
    line_name=$(trim_field "${raw_name:-}")

    if [ -z "$line_name" ]; then
        continue
    fi
    case "$line_name" in
        \#*)
            continue
            ;;
    esac

    line_source=$(trim_field "${raw_source:-}")
    line_description=$(trim_field "${raw_description:-}")
    line_argv0=$(trim_field "${raw_argv0:-tool}")
    line_cflags=$(trim_field "${raw_cflags:-}")

    if [ -z "$line_source" ] || [ -z "$line_description" ]; then
        echo "invalid manifest entry for tool '$line_name'" >&2
        exit 1
    fi
    if [ ! -f "$line_source" ]; then
        echo "tool source not found: $line_source" >&2
        exit 1
    fi

    case "$line_argv0" in
        tool)
            argv0_enum="ONETOOL_ARGV0_TOOL_NAME"
            ;;
        onetool)
            argv0_enum="ONETOOL_ARGV0_BINARY_PATH"
            ;;
        *)
            echo "unsupported argv0_mode '$line_argv0' for tool '$line_name'" >&2
            exit 1
            ;;
    esac

    symbol_suffix=$(sanitize_symbol "$line_name")
    entry_symbol="onetool_extra_${symbol_suffix}"
    object_path="$tool_obj_dir/${symbol_suffix}.o"

    printf 'int %s(int argc, char *argv[]);\n' "$entry_symbol" >> "$registry_tmp"

    if [ ! -f "$object_path" ] ||
       [ "$line_source" -nt "$object_path" ] ||
       [ "$manifest_path" -nt "$object_path" ] ||
       [ "$script_path" -nt "$object_path" ]; then
        compile_c_object "$object_path" "$line_source" "$line_cflags" "$entry_symbol"
    fi

    extra_objects="$extra_objects $object_path"
    extra_count=$((extra_count + 1))
done < "$manifest_path"

if [ "$extra_count" -eq 0 ]; then
    {
        printf '\nstatic const struct onetool_tool generated_extra_tools[] = {\n'
        printf '    { 0, 0, 0, ONETOOL_ARGV0_TOOL_NAME },\n'
        printf '};\n\n'
        printf 'const struct onetool_tool *onetool_extra_tools = generated_extra_tools;\n'
        printf 'const int onetool_extra_tool_count = 0;\n'
    } >> "$registry_tmp"
else
    {
        printf '\nstatic const struct onetool_tool generated_extra_tools[] = {\n'
    } >> "$registry_tmp"

    while IFS='|' read -r raw_name raw_source raw_description raw_argv0 raw_cflags; do
        line_name=$(trim_field "${raw_name:-}")

        if [ -z "$line_name" ]; then
            continue
        fi
        case "$line_name" in
            \#*)
                continue
                ;;
        esac

        line_description=$(trim_field "${raw_description:-}")
        line_argv0=$(trim_field "${raw_argv0:-tool}")
        symbol_suffix=$(sanitize_symbol "$line_name")
        entry_symbol="onetool_extra_${symbol_suffix}"
        escaped_name=$(escape_c_string "$line_name")
        escaped_description=$(escape_c_string "$line_description")

        if [ "$line_argv0" = "onetool" ]; then
            argv0_enum="ONETOOL_ARGV0_BINARY_PATH"
        else
            argv0_enum="ONETOOL_ARGV0_TOOL_NAME"
        fi

        printf '    { "%s", %s, "%s", %s },\n' \
            "$escaped_name" \
            "$entry_symbol" \
            "$escaped_description" \
            "$argv0_enum" >> "$registry_tmp"
    done < "$manifest_path"

    {
        printf '};\n\n'
        printf 'const struct onetool_tool *onetool_extra_tools = generated_extra_tools;\n'
        printf 'const int onetool_extra_tool_count = %d;\n' "$extra_count"
    } >> "$registry_tmp"
fi

write_if_changed "$registry_tmp" "$registry_c_path"

if [ ! -f "$registry_o_path" ] ||
   [ "$registry_c_path" -nt "$registry_o_path" ] ||
   [ "$header_path" -nt "$registry_o_path" ] ||
   [ "$script_path" -nt "$registry_o_path" ]; then
    compile_registry_object "$registry_o_path" "$registry_c_path"
fi

bundle_tmp="$tmp_dir/extra_tools_bundle.o.tmp"
set -- "$registry_o_path"
for object_path in $extra_objects; do
    set -- "$@" "$object_path"
done
"$CC" -r -o "$bundle_tmp" "$@"
write_if_changed "$bundle_tmp" "$bundle_obj_path"
