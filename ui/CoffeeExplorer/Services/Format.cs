using System;
using System.Globalization;
using System.Text;

namespace CoffeeExplorer.Services
{
    public static class Format
    {
        // Numbers are hex by default like everywhere in a PE editor. '0x' or a trailing 'h' also mean hex,
        // a trailing ' d' (or a leading '#') means decimal, the CFF Explorer go to dialog used the same notation.
        public static bool TryParseNumber(
            string? text,
            out ulong value
        )
        {
            value = 0;

            if (string.IsNullOrWhiteSpace(text))
            {
                return false;
            }

            string trimmed = text.Trim().Replace("_", string.Empty).Replace("`", string.Empty);
            bool is_decimal = false;

            if (trimmed.StartsWith("#", StringComparison.Ordinal))
            {
                is_decimal = true;
                trimmed = trimmed.Substring(1);
            }
            else if (trimmed.Length > 2 && char.IsWhiteSpace(trimmed[^2]) && (trimmed[^1] == 'd' || trimmed[^1] == 'D'))
            {
                is_decimal = true;
                trimmed = trimmed.Substring(0, trimmed.Length - 2).TrimEnd();
            }
            else if (trimmed.Length > 2 && char.IsWhiteSpace(trimmed[^2]) && (trimmed[^1] == 'h' || trimmed[^1] == 'H'))
            {
                trimmed = trimmed.Substring(0, trimmed.Length - 2).TrimEnd();
            }
            else if (trimmed.EndsWith("h", StringComparison.OrdinalIgnoreCase))
            {
                trimmed = trimmed.Substring(0, trimmed.Length - 1);
            }

            if (trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                is_decimal = false;
                trimmed = trimmed.Substring(2);
            }

            if (trimmed.Length == 0)
            {
                return false;
            }

            return is_decimal
                ? ulong.TryParse(trimmed, NumberStyles.None, CultureInfo.InvariantCulture, out value)
                : ulong.TryParse(trimmed, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out value);
        }

        // '1 detection', '3 detections'.
        public static string Plural(
            int count,
            string noun
        )
        {
            return count == 1 ? $"1 {noun}" : $"{count:N0} {noun}s";
        }

        public static string Hex(
            ulong value,
            int digits = 8
        )
        {
            return value.ToString("X" + digits, CultureInfo.InvariantCulture);
        }

        public static string Offset(
            ulong value
        )
        {
            return value > 0xFFFFFFFFul ? Hex(value, 16) : Hex(value, 8);
        }

        public static string SizeName(
            uint size
        )
        {
            return size switch
            {
                0 => string.Empty,
                1 => "Byte",
                2 => "Word",
                4 => "Dword",
                8 => "Qword",
                _ => $"Byte[{size}]"
            };
        }

        public static string Bytes(
            ulong size
        )
        {
            string[] units = { "bytes", "KB", "MB", "GB" };
            double value = size;
            int unit = 0;

            while (value >= 1024 && unit < units.Length - 1)
            {
                value /= 1024;
                ++unit;
            }

            return unit == 0 ? LanguageService.Tr("{0} bytes", size) :
                LanguageService.Tr("{0} {1} ({2} bytes)", value.ToString("0.##", CultureInfo.InvariantCulture), units[unit], size.ToString("N0", CultureInfo.InvariantCulture));
        }

        // Little endian bytes of a value, size 1 to 8.
        public static byte[] LittleEndian(
            ulong value,
            uint size
        )
        {
            var bytes = new byte[size];

            for (int index = 0; index < size && index < 8; ++index)
            {
                bytes[index] = (byte)(value >> (8 * index));
            }

            return bytes;
        }

        public static ulong MaxValue(
            uint size
        )
        {
            return size >= 8 ? ulong.MaxValue : (1ul << (int)(size * 8)) - 1;
        }

        // Fixed size 8 bit string, zero padded, cut at the field size.
        public static byte[] FixedString(
            string text,
            uint size
        )
        {
            var bytes = new byte[size];
            byte[] encoded = Encoding.Latin1.GetBytes(text);
            Array.Copy(encoded, bytes, Math.Min(encoded.Length, bytes.Length));
            return bytes;
        }

        public static string HexString(
            ReadOnlySpan<byte> data,
            string separator = " "
        )
        {
            var builder = new StringBuilder(data.Length * 3);

            for (int index = 0; index < data.Length; ++index)
            {
                if (index > 0)
                {
                    builder.Append(separator);
                }

                builder.Append(data[index].ToString("X2", CultureInfo.InvariantCulture));
            }

            return builder.ToString();
        }

        // '4D 5A 90', '4D5A90' or '4d, 5a' to bytes
        public static bool TryParseHexBytes(
            string text,
            out byte[] data
        )
        {
            var digits = new StringBuilder();

            foreach (char character in text)
            {
                if (Uri.IsHexDigit(character))
                {
                    digits.Append(character);
                }
                else if (!char.IsWhiteSpace(character) && character != ',' && character != '-')
                {
                    data = Array.Empty<byte>();
                    return false;
                }
            }

            if (digits.Length % 2 != 0)
            {
                data = Array.Empty<byte>();
                return false;
            }

            data = Convert.FromHexString(digits.ToString());
            return true;
        }
    }
}