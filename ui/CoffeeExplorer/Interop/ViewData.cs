using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

namespace CoffeeExplorer.Interop
{
    public sealed class TreeData
    {
        [JsonPropertyName("nodes")]
        public List<TreeNodeData> Nodes { get; set; } = new();
    }

    public sealed class TreeNodeData
    {
        [JsonPropertyName("id")]
        public string Id { get; set; } = string.Empty;

        [JsonPropertyName("name")]
        public string Name { get; set; } = string.Empty;

        // File, struct, list, folder, tool, group.
        [JsonPropertyName("kind")]
        public string Kind { get; set; } = string.Empty;

        [JsonPropertyName("children")]
        public List<TreeNodeData> Children { get; set; } = new();
    }

    public sealed class ViewData
    {
        [JsonPropertyName("node")]
        public string Node { get; set; } = string.Empty;

        [JsonPropertyName("title")]
        public string Title { get; set; } = string.Empty;

        // Fields, list, hex, overview, resources, empty.
        [JsonPropertyName("layout")]
        public string Layout { get; set; } = string.Empty;

        [JsonPropertyName("fields")]
        public List<FieldData>? Fields { get; set; }

        // Version strings of the overview.
        [JsonPropertyName("version")]
        public List<FieldData>? Version { get; set; }

        [JsonPropertyName("columns")]
        public List<ColumnData>? Columns { get; set; }

        [JsonPropertyName("rows")]
        public List<RowData>? Rows { get; set; }

        [JsonPropertyName("detail_layout")]
        public string? DetailLayout { get; set; }

        [JsonPropertyName("info")]
        public string? Info { get; set; }

        [JsonPropertyName("hex")]
        public HexData? Hex { get; set; }

        [JsonPropertyName("commands")]
        public List<CommandData>? Commands { get; set; }

        [JsonPropertyName("choice_sets")]
        public Dictionary<string, ChoiceSetData>? ChoiceSets { get; set; }

        [JsonPropertyName("warnings")]
        public List<string>? Warnings { get; set; }

        [JsonPropertyName("resources")]
        public List<ResourceData>? Resources { get; set; }

        // Image, icon, cursor, text, version, strings, hex.
        [JsonPropertyName("preview")]
        public string? Preview { get; set; }

        [JsonPropertyName("text")]
        public string? Text { get; set; }
    }

    public sealed class FieldData
    {
        [JsonPropertyName("name")]
        public string Name { get; set; } = string.Empty;

        [JsonPropertyName("offset")]
        public ulong? Offset { get; set; }

        [JsonPropertyName("size")]
        public uint Size { get; set; }

        [JsonPropertyName("type")]
        public string? Type { get; set; }

        [JsonPropertyName("value")]
        public string Value { get; set; } = string.Empty;

        [JsonPropertyName("meaning")]
        public string? Meaning { get; set; }

        // Hex, text, flags, enum, none.
        [JsonPropertyName("editor")]
        public string Editor { get; set; } = "none";

        [JsonPropertyName("choices")]
        public string? Choices { get; set; }

        [JsonPropertyName("tip")]
        public string? Tip { get; set; }

        // "entry" for the entry point, the row stands out.
        [JsonPropertyName("highlight")]
        public string? Highlight { get; set; }

        // Command fields (version information). No offset, an edit runs the command with the argument and the value.
        [JsonPropertyName("command")]
        public string? Command { get; set; }

        [JsonPropertyName("argument")]
        public string? Argument { get; set; }

        // Deferred overview values (hashes, signature). The member of the "overview_hashes" answer that fills the field.
        [JsonPropertyName("pending")]
        public string? Pending { get; set; }
    }

    public sealed class ColumnData
    {
        [JsonPropertyName("name")]
        public string Name { get; set; } = string.Empty;

        [JsonPropertyName("tip")]
        public string? Tip { get; set; }
    }

    public sealed class RowData
    {
        [JsonPropertyName("cells")]
        public List<CellData> Cells { get; set; } = new();

        [JsonPropertyName("detail")]
        public string? Detail { get; set; }

        [JsonPropertyName("navigate")]
        public string? Navigate { get; set; }

        [JsonPropertyName("offset")]
        public ulong? Offset { get; set; }

        [JsonPropertyName("size")]
        public ulong Size { get; set; }
    }

    public sealed class CellData
    {
        [JsonPropertyName("text")]
        public string Text { get; set; } = string.Empty;

        [JsonPropertyName("offset")]
        public ulong? Offset { get; set; }

        [JsonPropertyName("size")]
        public uint Size { get; set; }

        [JsonPropertyName("editor")]
        public string? Editor { get; set; }

        [JsonPropertyName("choices")]
        public string? Choices { get; set; }

        [JsonPropertyName("meaning")]
        public string? Meaning { get; set; }

        // Command cells. No offset, an edit runs the command with the argument and the value.
        [JsonPropertyName("command")]
        public string? Command { get; set; }

        [JsonPropertyName("argument")]
        public string? Argument { get; set; }
    }

    // Answer of the "entropy" query.
    public sealed class EntropyData
    {
        [JsonPropertyName("size")]
        public ulong Size { get; set; }

        [JsonPropertyName("block")]
        public ulong Block { get; set; }

        [JsonPropertyName("values")]
        public double[] Values { get; set; } = Array.Empty<double>();

        [JsonPropertyName("regions")]
        public List<EntropyRegion> Regions { get; set; } = new();
    }

    public sealed class EntropyRegion
    {
        [JsonPropertyName("name")]
        public string Name { get; set; } = string.Empty;

        // Headers, section, overlay or certificates.
        [JsonPropertyName("kind")]
        public string Kind { get; set; } = string.Empty;

        [JsonPropertyName("start")]
        public ulong Start { get; set; }

        [JsonPropertyName("end")]
        public ulong End { get; set; }
    }

    public sealed class HexData
    {
        [JsonPropertyName("offset")]
        public ulong Offset { get; set; }

        [JsonPropertyName("length")]
        public ulong Length { get; set; }

        [JsonPropertyName("title")]
        public string? Title { get; set; }
    }

    public sealed class CommandData
    {
        [JsonPropertyName("id")]
        public string Id { get; set; } = string.Empty;

        [JsonPropertyName("label")]
        public string Label { get; set; } = string.Empty;

        // View or row.
        [JsonPropertyName("scope")]
        public string Scope { get; set; } = "view";

        [JsonPropertyName("enabled")]
        public bool Enabled { get; set; } = true;

        [JsonPropertyName("reason")]
        public string? Reason { get; set; }
    }

    public sealed class ChoiceSetData
    {
        [JsonPropertyName("flags")]
        public bool Flags { get; set; }

        [JsonPropertyName("items")]
        public List<ChoiceItemData> Items { get; set; } = new();
    }

    public sealed class ChoiceItemData
    {
        // Hex without prefix.
        [JsonPropertyName("value")]
        public string Value { get; set; } = "0";

        [JsonPropertyName("name")]
        public string Name { get; set; } = string.Empty;

        // Hex mask of a multi bit field inside a flags value (alignment, subsystem bits etc).
        [JsonPropertyName("mask")]
        public string? Mask { get; set; }
    }

    public sealed class ResourceData
    {
        [JsonPropertyName("path")]
        public string Path { get; set; } = string.Empty;

        [JsonPropertyName("type")]
        public string Type { get; set; } = string.Empty;

        [JsonPropertyName("type_name")]
        public string TypeName { get; set; } = string.Empty;

        [JsonPropertyName("name")]
        public string Name { get; set; } = string.Empty;

        [JsonPropertyName("language")]
        public uint Language { get; set; }

        [JsonPropertyName("language_name")]
        public string? LanguageName { get; set; }

        [JsonPropertyName("rva")]
        public string? Rva { get; set; }

        [JsonPropertyName("size")]
        public uint Size { get; set; }

        [JsonPropertyName("offset")]
        public ulong? Offset { get; set; }

        [JsonPropertyName("preview")]
        public string Preview { get; set; } = "hex";
    }

    public sealed class CommandAnswer
    {
        [JsonPropertyName("ok")]
        public bool Ok { get; set; }

        [JsonPropertyName("message")]
        public string? Message { get; set; }

        [JsonPropertyName("select")]
        public string? Select { get; set; }
    }

    [JsonSerializable(typeof(ViewData))]
    [JsonSerializable(typeof(TreeData))]
    [JsonSerializable(typeof(EntropyData))]
    [JsonSerializable(typeof(CommandAnswer))]
    internal sealed partial class CoreJsonContext : JsonSerializerContext
    {
    }

    public static class CoreJson
    {
        private static JsonTypeInfo<T> TypeInfo<T>()
        {
            return (JsonTypeInfo<T>)CoreJsonContext.Default.GetTypeInfo(typeof(T))!;
        }

        public static T Parse<T>(
            string text
        ) where T : new()
        {
            return CoreTranslation.Translate(JsonSerializer.Deserialize(text, TypeInfo<T>()) ?? new T());
        }

        // UTF-8 straight from the core, no string in between.
        public static T Parse<T>(
            byte[] utf8
        ) where T : new()
        {
            return CoreTranslation.Translate(JsonSerializer.Deserialize(utf8, TypeInfo<T>()) ?? new T());
        }

        public static T Parse<T>(
            JsonDocument document
        ) where T : new()
        {
            return CoreTranslation.Translate(document.RootElement.Deserialize(TypeInfo<T>()) ?? new T());
        }

        public static string GetString(
            JsonElement element,
            string name,
            string fallback = ""
        )
        {
            return element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String
                ? value.GetString() ?? fallback
                : fallback;
        }

        public static ulong GetUnsigned(
            JsonElement element,
            string name,
            ulong fallback = 0
        )
        {
            return element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number &&
                   value.TryGetUInt64(out ulong number)
                ? number
                : fallback;
        }

        public static bool GetBool(
            JsonElement element,
            string name,
            bool fallback = false
        )
        {
            if (element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out JsonElement value))
            {
                return value.ValueKind == JsonValueKind.True || (value.ValueKind != JsonValueKind.False && fallback);
            }

            return fallback;
        }
    }
}