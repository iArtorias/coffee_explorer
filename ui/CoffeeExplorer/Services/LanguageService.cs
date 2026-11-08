using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Data;
using System.Windows.Markup;

namespace CoffeeExplorer.Services
{
    public sealed class LanguageInfo
    {
        public LanguageInfo(
            string id,
            string name
        )
        {
            Id = id;
            Name = name;
        }

        // File name without .lang, 'English' for the built in strings.
        public string Id { get; }

        // Native name from the '@Name' line of the file.
        public string Name { get; }

        public override string ToString()
        {
            return Name;
        }
    }

    // Interface translations. The english text in the code is both the key and the fallback, a translation is a
    // 'translations\<Language>.lang' file next to the executable with lines of the form
    //
    //  English text=Translated text
    //
    // Lines starting with '#' are comments, '@Name=' gives the native name shown in the options, '@Culture=' the culture
    // (de-DE) used for the UI culture. '\n', '\t', '\=' and '\\' are escapes. A missing line keeps the english text.
    public sealed class LanguageService : INotifyPropertyChanged
    {
        public const string ENGLISH = "English";
        public const string FOLDER_NAME = "translations";
        private const string EXTENSION = ".lang";

        private Dictionary<string, string> m_Strings = new(StringComparer.Ordinal);
        // Keys with {0}, {1} placeholders, matched against text the core composes ('Section {0}').
        private TemplateIndex m_Templates = new(new List<Template>());
        private int m_Version;

        public static LanguageService Instance { get; } = new();

        public static string FolderPath => Path.Combine(AppContext.BaseDirectory, FOLDER_NAME);

        public static string Current { get; private set; } = ENGLISH;

        public static event EventHandler? LanguageChanged;

        public event PropertyChangedEventHandler? PropertyChanged;

        // Bumped on every change, XAML bindings to it rerun their converter and pick up the new text.
        public int Version => m_Version;

        public string this[string text] => Tr(text);

        public static string Tr(
            string text
        )
        {
            return Instance.m_Strings.TryGetValue(text, out string? translated) && translated.Length > 0 ? translated : text;
        }

        // Format string with {0} placeholders, the translation keeps the placeholders
        public static string Tr(
            string format,
            params object?[] arguments
        )
        {
            try
            {
                return string.Format(CultureInfo.CurrentCulture, Tr(format), arguments);
            }
            catch (FormatException)
            {
                // A broken translation (missing or extra placeholder) falls back to english.
                return string.Format(CultureInfo.CurrentCulture, format, arguments);
            }
        }

        public static bool IsEnglish => Instance.m_Strings.Count == 0;

        // Text of the core (view titles, tips, info, messages). The exact key, then the {0} templates, then line by line.
        public static string TrText(
            string? text
        )
        {
            return text == null ? string.Empty : TrText(text, 0);
        }

        // A comma separated list of names, such as the meaning of a flags value ('Is readable (MEM_READ), MEM_WRITE').
        public static string TrList(
            string? text
        )
        {
            if (string.IsNullOrEmpty(text) || IsEnglish)
            {
                return text ?? string.Empty;
            }

            if (TryTranslate(text, 0, out string whole))
            {
                return whole;
            }

            string[] parts = text.Split(", ");

            if (parts.Length < 2)
            {
                return text;
            }

            // Names can contain ', ' themselves, the longest run of parts that is a key wins.
            var result = new List<string>();
            bool changed = false;

            for (int start = 0; start < parts.Length;)
            {
                int end = parts.Length;
                string? translated = null;

                for (; end > start; --end)
                {
                    if (Instance.m_Strings.TryGetValue(string.Join(", ", parts, start, end - start), out translated) && translated.Length > 0)
                    {
                        break;
                    }
                }

                if (end > start && translated != null)
                {
                    result.Add(translated);
                    changed = true;
                    start = end;
                }
                else
                {
                    result.Add(parts[start]);
                    ++start;
                }
            }

            return changed ? string.Join(", ", result) : text;
        }

        // Exact key only, for names that can be data (column headers, choice names, language names).
        public static bool TryExact(
            string text,
            out string translated
        )
        {
            if (Instance.m_Strings.TryGetValue(text, out string? value) && value.Length > 0)
            {
                translated = value;
                return true;
            }

            translated = text;
            return false;
        }

        private static string TrText(
            string text,
            int depth
        )
        {
            if (text.Length == 0 || IsEnglish)
            {
                return text;
            }

            // Multi line text of the core (section contents, rebuilder log). The whole text as a key, else every line on its own.
            // Templates never span lines.
            if (text.IndexOf('\n') < 0)
            {
                return TryTranslate(text, depth, out string translated) ? translated : text;
            }

            if (TryExact(text, out string whole))
            {
                return whole;
            }

            string[] lines = text.Split('\n');
            bool changed = false;

            for (int index = 0; index < lines.Length; ++index)
            {
                string line = lines[index];
                string trimmed = line.TrimStart();

                if (trimmed.Length > 0 && TryTranslate(trimmed.TrimEnd('\r'), depth, out string line_text))
                {
                    lines[index] = line.Substring(0, line.Length - trimmed.Length) + line_text;
                    changed = true;
                }
            }

            if (changed)
            {
                return string.Join("\n", lines);
            }

            return text;
        }

        private static bool TryTranslate(
            string text,
            int depth,
            out string translated
        )
        {
            if (TryExact(text, out translated))
            {
                return true;
            }

            foreach (Template template in Instance.m_Templates.Candidates(text))
            {
                if (!text.StartsWith(template.Prefix, StringComparison.Ordinal) || !text.EndsWith(template.Suffix, StringComparison.Ordinal))
                {
                    continue;
                }

                Match match = template.Pattern.Match(text);

                if (!match.Success)
                {
                    continue;
                }

                // The values inside are translated too ('Export Directory (beyond 'NumberOfRvaAndSizes')'), two levels deep.
                var builder = new StringBuilder(template.Translation);

                for (int index = template.Count - 1; index >= 0; --index)
                {
                    string value = match.Groups["p" + index].Value;
                    builder.Replace("{" + index + "}", depth < 2 ? TrText(value, depth + 1) : value);
                }

                translated = builder.ToString();
                return true;
            }

            translated = text;
            return false;
        }

        // Templates bucketed by the first character of their literal prefix (or the last of the suffix), a list cell
        // is checked against a handful of them instead of all.
        private sealed class TemplateIndex
        {
            private readonly Dictionary<char, List<Template>> m_ByFirst = new();
            private readonly Dictionary<char, List<Template>> m_ByLast = new();
            private readonly List<Template> m_Anywhere = new();

            public TemplateIndex(
                List<Template> templates
            )
            {
                foreach (Template template in templates)
                {
                    if (template.Prefix.Length > 0)
                    {
                        Bucket(m_ByFirst, template.Prefix[0]).Add(template);
                    }
                    else if (template.Suffix.Length > 0)
                    {
                        Bucket(m_ByLast, template.Suffix[^1]).Add(template);
                    }
                    else
                    {
                        m_Anywhere.Add(template);
                    }
                }
            }

            private static List<Template> Bucket(
                Dictionary<char, List<Template>> buckets,
                char key
            )
            {
                if (!buckets.TryGetValue(key, out List<Template>? list))
                {
                    list = new List<Template>();
                    buckets[key] = list;
                }

                return list;
            }

            // Most specific first within each group, the groups are disjoint so their order does not matter.
            public IEnumerable<Template> Candidates(
                string text
            )
            {
                if (text.Length == 0)
                {
                    yield break;
                }

                if (m_ByFirst.TryGetValue(text[0], out List<Template>? first))
                {
                    foreach (Template template in first)
                    {
                        yield return template;
                    }
                }

                if (m_ByLast.TryGetValue(text[^1], out List<Template>? last))
                {
                    foreach (Template template in last)
                    {
                        yield return template;
                    }
                }

                foreach (Template template in m_Anywhere)
                {
                    yield return template;
                }
            }
        }

        private sealed class Template
        {
            public Template(
                string key,
                string translation
            )
            {
                Translation = translation;
                var pattern = new StringBuilder("^");
                int position = 0;
                bool first = true;
                var defined = new HashSet<int>();

                // '{0}' to '{9}', scanned by hand. A language with a few hundred templates used to build as many regexes at startup.
                for (int index = 0; index + 2 < key.Length; ++index)
                {
                    if (!IsPlaceholderAt(key, index))
                    {
                        continue;
                    }

                    string literal = key.Substring(position, index - position);

                    if (first)
                    {
                        Prefix = literal;
                        first = false;
                    }

                    pattern.Append(Regex.Escape(literal));
                    int number = key[index + 1] - '0';

                    // A placeholder that comes twice must match the same text.
                    pattern.Append(defined.Contains(number) ? $"\\k<p{number}>" : $"(?<p{number}>.+?)");
                    defined.Add(number);
                    Count = Math.Max(Count, number + 1);
                    position = index + 3;
                    index += 2;
                }

                Suffix = key.Substring(position);
                pattern.Append(Regex.Escape(Suffix)).Append('$');
                m_PatternText = pattern.ToString();
                LiteralLength = key.Length - Count * 3;
            }

            private readonly string m_PatternText;
            private Regex? m_Pattern;

            private static bool IsPlaceholderAt(
                string text,
                int index
            )
            {
                return index + 2 < text.Length && text[index] == '{' && char.IsAsciiDigit(text[index + 1]) && text[index + 2] == '}';
            }

            public static bool HasPlaceholder(
                string text
            )
            {
                for (int index = 0; index + 2 < text.Length; ++index)
                {
                    if (IsPlaceholderAt(text, index))
                    {
                        return true;
                    }
                }

                return false;
            }

            public static string RemovePlaceholders(
                string text
            )
            {
                var result = new StringBuilder(text.Length);

                for (int index = 0; index < text.Length; ++index)
                {
                    if (IsPlaceholderAt(text, index))
                    {
                        index += 2;
                        continue;
                    }

                    result.Append(text[index]);
                }

                return result.ToString();
            }

            public string Prefix { get; } = string.Empty;

            public string Suffix { get; }

            // Built on first use, most templates are never needed in a session. Two threads may both build it, both are equal.
            public Regex Pattern => m_Pattern ??= new Regex(m_PatternText, RegexOptions.CultureInvariant);

            public string Translation { get; }

            public int Count { get; }

            public int LiteralLength { get; }
        }

        public static IReadOnlyList<LanguageInfo> Available()
        {
            var list = new List<LanguageInfo> { new(ENGLISH, ENGLISH) };

            try
            {
                if (Directory.Exists(FolderPath))
                {
                    foreach (string path in Directory.GetFiles(FolderPath, "*" + EXTENSION).OrderBy(path => path, StringComparer.OrdinalIgnoreCase))
                    {
                        string id = Path.GetFileNameWithoutExtension(path);

                        if (!string.Equals(id, ENGLISH, StringComparison.OrdinalIgnoreCase))
                        {
                            string name = ReadHeader(path, "@Name") ?? id;
                            list.Add(new LanguageInfo(id, name));
                        }
                    }
                }
            }
            catch (Exception)
            {
                // An unreadable folder leaves english only.
            }

            return list;
        }

        // Loads 'translations\<id>.lang', an unknown or broken file falls back to english.
        public static void Apply(
            string? id
        )
        {
            var strings = new Dictionary<string, string>(StringComparer.Ordinal);
            string applied = ENGLISH;
            string? culture = null;

            if (!string.IsNullOrWhiteSpace(id) && !string.Equals(id, ENGLISH, StringComparison.OrdinalIgnoreCase))
            {
                string path = Path.Combine(FolderPath, id + EXTENSION);

                try
                {
                    if (File.Exists(path))
                    {
                        foreach (string line in File.ReadAllLines(path, Encoding.UTF8))
                        {
                            if (ParseLine(line, out string key, out string value))
                            {
                                if (key == "@Culture")
                                {
                                    culture = value;
                                }
                                else if (!key.StartsWith('@'))
                                {
                                    strings[key] = value;
                                }
                            }
                        }

                        applied = id;
                    }
                }
                catch (Exception error)
                {
                    Log.Error(error);
                    strings.Clear();
                    applied = ENGLISH;
                }
            }

            // Templates need a literal part, a bare '{0}' would match everything. The most specific one is tried first.
            var templates = strings
                .Where(pair => pair.Value.Length > 0 && Template.HasPlaceholder(pair.Key) && Template.RemovePlaceholders(pair.Key).Trim().Length >= 3)
                .Select(pair => new Template(pair.Key, pair.Value))
                .OrderByDescending(template => template.LiteralLength)
                .ToList();

            ApplyCulture(applied == ENGLISH ? "en-US" : culture);
            Instance.m_Strings = strings;
            Instance.m_Templates = new TemplateIndex(templates);
            Current = applied;
            ++Instance.m_Version;
            Instance.PropertyChanged?.Invoke(Instance, new PropertyChangedEventArgs(nameof(Version)));
            Instance.PropertyChanged?.Invoke(Instance, new PropertyChangedEventArgs("Item[]"));
            LanguageChanged?.Invoke(null, EventArgs.Empty);
        }

        private static void ApplyCulture(
            string? name
        )
        {
            if (string.IsNullOrWhiteSpace(name))
            {
                return;
            }

            try
            {
                // Only the UI culture. Number parsing in the dialogs stays invariant or current as before.
                CultureInfo.CurrentUICulture = CultureInfo.DefaultThreadCurrentUICulture = CultureInfo.GetCultureInfo(name);
            }
            catch (CultureNotFoundException)
            {
            }
        }

        private static string? ReadHeader(
            string path,
            string header
        )
        {
            foreach (string line in File.ReadLines(path, Encoding.UTF8).Take(40))
            {
                if (ParseLine(line, out string key, out string value) && key == header && value.Length > 0)
                {
                    return value;
                }
            }

            return null;
        }

        private static bool ParseLine(
            string line,
            out string key,
            out string value
        )
        {
            key = value = string.Empty;
            string text = line.Trim();

            if (text.Length == 0 || text[0] == '#')
            {
                return false;
            }

            // The first '=' that is not escaped separates the english text from the translation.
            int split = -1;

            for (int index = 0; index < text.Length; ++index)
            {
                if (text[index] == '\\')
                {
                    ++index;
                }
                else if (text[index] == '=')
                {
                    split = index;
                    break;
                }
            }

            if (split <= 0)
            {
                return false;
            }

            key = Unescape(text.Substring(0, split).Trim());
            value = Unescape(text.Substring(split + 1).Trim());
            return key.Length > 0;
        }

        private static string Unescape(
            string text
        )
        {
            if (text.IndexOf('\\') < 0)
            {
                return text;
            }

            var builder = new StringBuilder(text.Length);

            for (int index = 0; index < text.Length; ++index)
            {
                char current = text[index];

                if (current == '\\' && index + 1 < text.Length)
                {
                    char next = text[++index];
                    builder.Append(next switch { 'n' => '\n', 't' => '\t', _ => next });
                }
                else
                {
                    builder.Append(current);
                }
            }

            return builder.ToString();
        }
    }

    // {l:Tr 'English text'} in XAML. The text follows the selected language live.
    [MarkupExtensionReturnType(typeof(object))]
    public sealed class TrExtension : MarkupExtension
    {
        public TrExtension()
        {
        }

        public TrExtension(
            string text
        )
        {
            Text = text;
        }

        [ConstructorArgument("text")]
        public string Text { get; set; } = string.Empty;

        public override object ProvideValue(
            IServiceProvider service_provider
        )
        {
            var binding = new Binding(nameof(LanguageService.Version))
            {
                Source = LanguageService.Instance,
                Mode = BindingMode.OneWay,
                Converter = TranslateConverter.Instance,
                ConverterParameter = Text
            };

            return binding.ProvideValue(service_provider);
        }
    }

    public sealed class TranslateConverter : IValueConverter
    {
        public static TranslateConverter Instance { get; } = new();

        public object Convert(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            return LanguageService.Tr(parameter as string ?? string.Empty);
        }

        public object ConvertBack(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            throw new NotSupportedException();
        }
    }

    // Formats the bound value with a translated pattern, '#' stands for the value. ConverterParameter='# bytes'.
    public sealed class TrFormatConverter : IValueConverter
    {
        public static TrFormatConverter Instance { get; } = new();

        public object Convert(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            string key = (parameter as string ?? "#").Replace("#", "{0}");
            return LanguageService.Tr(key, value);
        }

        public object ConvertBack(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            throw new NotSupportedException();
        }
    }
}