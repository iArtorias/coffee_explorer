using System;
using System.Collections.Generic;
using CoffeeExplorer.Services;

namespace CoffeeExplorer.Interop
{
    // The core writes English text into the view protocol (tree, titles, field names, column headers, tips, commands,
    // warnings). The translation happens here, right after parsing, so the views and the core stay language neutral.
    // Data is never translated. Values, names read from the file and editable cells stay as they are.
    internal static class CoreTranslation
    {
        // Views whose read only cells hold descriptive text of the core ("(by ordinal)", directory names, unwind kinds).
        // Everywhere else a cell is data (export names, resource strings, metadata) and an exact key could hit by chance.
        private static readonly HashSet<string> TEXT_CELL_VIEWS = new(StringComparer.Ordinal)
        {
            "datadirs", "sections", "exports", "imports", "exceptions", "security", "relocs", "debug", "tls", "bound", "iat", "delay"
        };

        // Views listing function and module names. A single word there is a name ("Copy", "Data"), only phrases are translated.
        private static readonly HashSet<string> NAME_VIEWS = new(StringComparer.Ordinal)
        {
            "exports", "imports", "iat", "delay", "bound", "exceptions"
        };

        public static T Translate<T>(
            T value
        )
        {
            if (LanguageService.IsEnglish)
            {
                return value;
            }

            switch (value)
            {
                case ViewData view:
                    TranslateView(view);
                    break;
                case TreeData tree:
                    TranslateNodes(tree.Nodes);
                    break;
                case EntropyData entropy:
                    foreach (EntropyRegion region in entropy.Regions)
                    {
                        region.Name = LanguageService.TryExact(region.Name, out string name) ? name : region.Name;
                    }

                    break;
                case CommandAnswer answer when answer.Message != null:
                    answer.Message = LanguageService.TrText(answer.Message);
                    break;
            }

            return value;
        }

        private static void TranslateNodes(
            List<TreeNodeData> nodes
        )
        {
            foreach (TreeNodeData node in nodes)
            {
                node.Name = LanguageService.TrText(node.Name);
                TranslateNodes(node.Children);
            }
        }

        private static void TranslateView(
            ViewData view
        )
        {
            view.Title = LanguageService.TrText(view.Title);

            if (view.Info != null)
            {
                view.Info = LanguageService.TrText(view.Info);
            }

            if (view.Hex?.Title != null)
            {
                view.Hex.Title = LanguageService.TrText(view.Hex.Title);
            }

            // Resource tree. Language names ("English (United States)"), type names stay, the icons are chosen by them and the groups translate them.
            if (view.Resources != null)
            {
                foreach (ResourceData resource in view.Resources)
                {
                    if (resource.LanguageName != null)
                    {
                        resource.LanguageName = LanguageService.TryExact(resource.LanguageName, out string language) ? language : resource.LanguageName;
                    }
                }
            }

            if (view.Warnings != null)
            {
                for (int index = 0; index < view.Warnings.Count; ++index)
                {
                    view.Warnings[index] = LanguageService.TrText(view.Warnings[index]);
                }
            }

            TranslateFields(view.Fields, view.Layout == "overview");

            // Version strings are the file's own text, only the names of the fixed part have keys.
            if (view.Version != null)
            {
                foreach (FieldData field in view.Version)
                {
                    field.Name = LanguageService.TryExact(field.Name, out string name) ? name : field.Name;
                }
            }

            if (view.Columns != null)
            {
                foreach (ColumnData column in view.Columns)
                {
                    column.Name = LanguageService.TryExact(column.Name, out string name) ? name : column.Name;
                    column.Tip = column.Tip != null ? LanguageService.TrText(column.Tip) : null;
                }
            }

            if (view.Rows != null)
            {
                string root = view.Node.Split('/')[0];
                bool text_cells = TEXT_CELL_VIEWS.Contains(root);
                bool names = NAME_VIEWS.Contains(root);

                foreach (RowData row in view.Rows)
                {
                    foreach (CellData cell in row.Cells)
                    {
                        if (cell.Meaning != null)
                        {
                            cell.Meaning = LanguageService.TrList(cell.Meaning);
                        }

                        // Editable cells and command cells are values of the file.
                        if (text_cells && cell.Offset == null && cell.Command == null && cell.Text.Length > 0 && (!names || cell.Text.Contains(' ')) &&
                             !LooksLikeData(cell.Text))
                        {
                            cell.Text = LanguageService.TrText(cell.Text);
                        }
                    }
                }
            }

            if (view.Commands != null)
            {
                foreach (CommandData command in view.Commands)
                {
                    command.Label = LanguageService.TrText(command.Label);

                    if (command.Reason != null)
                    {
                        command.Reason = LanguageService.TrText(command.Reason);
                    }
                }
            }

            if (view.ChoiceSets != null)
            {
                foreach (ChoiceSetData set in view.ChoiceSets.Values)
                {
                    foreach (ChoiceItemData item in set.Items)
                    {
                        item.Name = LanguageService.TryExact(item.Name, out string name) ? name : item.Name;
                    }
                }
            }
        }

        private static void TranslateFields(
            List<FieldData>? fields,
            bool overview
        )
        {
            if (fields == null)
            {
                return;
            }

            foreach (FieldData field in fields)
            {
                // Structure members ('e_lfanew', 'SizeOfImage') keep their names, descriptive rows ("File size", "Name (string)") are translated.
                if (field.Offset == null || field.Name.Contains(' '))
                {
                    field.Name = LanguageService.TrText(field.Name);
                }

                if (field.Tip != null)
                {
                    field.Tip = LanguageService.TrText(field.Tip);
                }

                if (field.Meaning != null)
                {
                    field.Meaning = LanguageService.TrList(LanguageService.TrText(field.Meaning));
                }

                // Property rows of the overview and other info rows. The value is text of the core ("Not signed", "None"),
                // file names, hashes and versions match no key and stay as they are.
                if (field.Offset == null && field.Editor == "none" && field.Value.Length > 0 && (overview || !LooksLikeData(field.Value)))
                {
                    field.Value = LanguageService.TrList(LanguageService.TrText(field.Value));
                }
            }
        }

        private static bool LooksLikeData(
            string value
        )
        {
            // Hex numbers and hashes.
            foreach (char character in value)
            {
                if (!Uri.IsHexDigit(character) && character != ' ' && character != 'h')
                {
                    return false;
                }
            }

            return true;
        }
    }
}