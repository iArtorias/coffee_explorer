using System;
using System.Collections.Generic;
using System.IO;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Services;
using CoffeeExplorer.Views.Dialogs;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.ViewModels
{
    // Commands of the views (context menus of section headers, imports, resources etc). The core lists them
    // with 'commands', the UI asks for the arguments and runs PcExecute.
    public sealed partial class DocumentViewModel
    {
        private static string ALL_FILES => Tr("All files") + " (*.*)|*.*";

        public Action<CommandData> RunViewCommand(
            NodeViewModel node
        )
        {
            return command => RunNodeCommand(node, command);
        }

        private void RunNodeCommand(
            NodeViewModel node,
            CommandData command
        )
        {
            TableRow? row = node.List?.SelectedRow;

            if (command.Scope == "row" && row == null)
            {
                m_Main.SetStatus(Tr("Select a row first"));
                return;
            }

            int index = row?.Index ?? -1;
            string name = row != null && row.Cells.Count > 0 ? row.Cells[0].Text : string.Empty;

            switch (command.Id)
            {
                case "section.add_empty":
                case "section.add_header":
                case "section.add_file":
                    {
                        var dialog = new AddSectionDialog(command.Id == "section.add_file", command.Id == "section.add_header");

                        if (!dialog.ShowFor(m_Main.Window))
                        {
                            return;
                        }

                        var request = new Dictionary<string, object>
                        {
                            ["command"] = command.Id,
                            ["name"] = dialog.SectionName,
                            ["size"] = dialog.SectionSize,
                            ["characteristics"] = dialog.Characteristics
                        };

                        if (dialog.FilePath != null)
                        {
                            request["path"] = dialog.FilePath;
                        }

                        Execute(request);
                        return;
                    }

                case "section.delete":
                case "section.delete_header":
                    {
                        string what = command.Id == "section.delete"
                            ? Tr("Delete section {0} and its raw data? The data of the following sections moves up.", name)
                            : Tr("Delete the header of section {0}? The raw data stays in the file.", name);

                        if (MessageService.Confirm(m_Main.Window, Tr("Delete section"), what, Tr("Delete"), Tr("Cancel"), MessageKind.MESSAGE_WARNING))
                        {
                            Execute(new Dictionary<string, object> { ["command"] = command.Id, ["index"] = index });
                        }

                        return;
                    }

                case "section.replace":
                    {
                        string? path = FileDialogs.Open(Tr("Replace the data of {0}", name), ALL_FILES);

                        if (path != null)
                        {
                            Execute(new Dictionary<string, object> { ["command"] = command.Id, ["index"] = index, ["path"] = path });
                        }

                        return;
                    }

                case "section.dump":
                    SaveRegion($"sections/{index}", SafeFileName(FileNameWithoutExtension() + "_" + name.Trim('.')) + ".bin");
                    return;

                case "security.save":
                    SaveRegion(row?.Detail ?? $"security/{index}", FileNameWithoutExtension() + "_signature.p7b");
                    return;

                case "overlay.save":
                    SaveRegion("overlay", FileNameWithoutExtension() + "_overlay.bin");
                    return;

                case "imports.delete":
                    if (MessageService.Confirm(m_Main.Window, Tr("Delete import descriptor"), Tr("Remove {0} from the import directory? The loader will no longer load it.", name), Tr("Delete"), Tr("Cancel"), MessageKind.MESSAGE_WARNING))
                    {
                        Execute(new Dictionary<string, object> { ["command"] = command.Id, ["index"] = index });
                    }

                    return;

                case "security.remove":
                    if (MessageService.Confirm(m_Main.Window, Tr("Remove signature"), Tr("Remove the Authenticode signature? Windows will treat the file as unsigned."), Tr("Remove"), Tr("Cancel"), MessageKind.MESSAGE_WARNING))
                    {
                        Execute(new Dictionary<string, object> { ["command"] = command.Id });
                    }

                    return;

                case "symbols.load":
                    LoadSymbols(null);
                    return;

                case "symbols.browse":
                    BrowseSymbols();
                    return;

                case "symbols.download":
                    DownloadSymbols();
                    return;

                case "rich.clear":
                    if (MessageService.Confirm(m_Main.Window, Tr("Remove Rich header"), Tr("Clear the Rich header (build tool records of the linker) to zero? Undo brings it back."), Tr("Remove"), Tr("Cancel"), MessageKind.MESSAGE_WARNING))
                    {
                        Execute(new Dictionary<string, object> { ["command"] = command.Id });
                    }

                    return;

                case "debug.remove":
                    if (MessageService.Confirm(m_Main.Window, Tr("Remove debug directory"),
                                                 Tr("Remove the debug directory with all its entries and clear their data (PDB path, POGO, VC feature, extended DLL characteristics such as CET compatibility)? Debuggers will no longer find the PDB. Undo brings it back."),
                                                 Tr("Remove"), Tr("Cancel"), MessageKind.MESSAGE_WARNING))
                    {
                        Execute(new Dictionary<string, object> { ["command"] = command.Id });
                    }

                    return;

                case "overlay.remove":
                    if (MessageService.Confirm(m_Main.Window, Tr("Remove overlay"), Tr("Cut the data after the last section from the file? Installers and self extracting archives stop working."), Tr("Remove"), Tr("Cancel"), MessageKind.MESSAGE_WARNING))
                    {
                        Execute(new Dictionary<string, object> { ["command"] = command.Id });
                    }

                    return;
            }

            // Everything else takes the selected row index (imports.move_up, rebuild.image_size etc).
            var generic = new Dictionary<string, object> { ["command"] = command.Id };

            if (index >= 0)
            {
                generic["index"] = index;
            }

            CommandAnswer? answer = Execute(generic);

            // Keep the moved import descriptor selected.
            if (answer != null && Content is NodeViewModel current && current.List != null && index >= 0)
            {
                int target = command.Id == "imports.move_up" ? index - 1 : command.Id == "imports.move_down" ? index + 1 : index;

                if (target >= 0 && target < current.List.Rows.Count)
                {
                    current.List.SelectedRow = current.List.Rows[target];
                }
            }
        }

        public void RunResourceCommand(
            CommandData command,
            ResourceItemViewModel? item
        )
        {
            if (command.Scope == "row" && item == null)
            {
                m_Main.SetStatus(Tr("Select a resource first"));
                return;
            }

            switch (command.Id)
            {
                case "resources.add":
                    {
                        var dialog = new AddResourceDialog();

                        if (dialog.ShowFor(m_Main.Window))
                        {
                            Execute(new Dictionary<string, object>
                            {
                                ["command"] = command.Id,
                                ["type"] = dialog.ResourceType,
                                ["name"] = dialog.ResourceName,
                                ["language"] = dialog.LanguageId,
                                ["path"] = dialog.FilePath
                            });
                        }

                        return;
                    }

                case "resources.add_icon":
                case "resources.replace_icon":
                    {
                        string? path = FileDialogs.Open(command.Id == "resources.add_icon" ? Tr("Add icon") : Tr("Replace icon"), Tr("Icons and cursors") + " (*.ico;*.cur)|*.ico;*.cur|" + ALL_FILES);

                        if (path == null)
                        {
                            return;
                        }

                        var request = new Dictionary<string, object> { ["command"] = command.Id, ["path"] = path };

                        if (item != null)
                        {
                            request["index"] = item.Index;
                            request["language"] = item.Data.Language;
                        }

                        Execute(request);
                        return;
                    }

                case "resources.add_bitmap":
                    {
                        string? path = FileDialogs.Open(Tr("Add bitmap"), Tr("Bitmaps") + " (*.bmp)|*.bmp|" + ALL_FILES);

                        if (path != null)
                        {
                            Execute(new Dictionary<string, object> { ["command"] = command.Id, ["path"] = path });
                        }

                        return;
                    }

                case "resources.replace":
                    {
                        string? path = FileDialogs.Open(Tr("Replace {0} / {1}", TrText(item!.Data.TypeName), item.Data.Name), ALL_FILES);

                        if (path != null)
                        {
                            Execute(new Dictionary<string, object> { ["command"] = command.Id, ["index"] = item.Index, ["path"] = path });
                        }

                        return;
                    }

                case "resources.remove":
                    if (MessageService.Confirm(m_Main.Window, Tr("Remove resource"), Tr("Remove {0} / {1} / {2}?", TrText(item!.Data.TypeName), item.Data.Name, item.Data.Language), Tr("Remove"), Tr("Cancel"), MessageKind.MESSAGE_WARNING))
                    {
                        Execute(new Dictionary<string, object> { ["command"] = command.Id, ["index"] = item.Index });
                    }

                    return;

                case "resources.save":
                    SaveResource(item!);
                    return;

                case "resources.import":
                    {
                        string? path = FileDialogs.Open(Tr("Import resources from"), Tr("Portable executables") + " (*.exe;*.dll;*.sys;*.ocx;*.mui)|*.exe;*.dll;*.sys;*.ocx;*.mui|" + ALL_FILES);

                        if (path == null)
                        {
                            return;
                        }

                        bool replace = MessageService.Confirm(m_Main.Window, Tr("Import resources"),
                            Tr("Replace all resources of this file with the ones of the source file, or merge them (same type, name and language are replaced)?"),
                            Tr("Replace all"), Tr("Merge"), MessageKind.MESSAGE_QUESTION);
                        Execute(new Dictionary<string, object> { ["command"] = command.Id, ["path"] = path, ["replace"] = replace });
                        return;
                    }
            }

            var generic = new Dictionary<string, object> { ["command"] = command.Id };

            if (item != null)
            {
                generic["index"] = item.Index;
            }

            Execute(generic);
        }

        private void SaveResource(
            ResourceItemViewModel item
        )
        {
            try
            {
                byte[] data = Document.ExportResource(item.Index, false, out string extension);
                string name = SafeFileName($"{FileNameWithoutExtension()}_{item.Data.TypeName}_{item.Data.Name}") + (extension.Length > 0 ? extension : ".bin");
                string? path = FileDialogs.Save(Tr("Save resource"), name, $"*{extension}|*{extension}|" + ALL_FILES);

                if (path != null)
                {
                    File.WriteAllBytes(path, data);
                    m_Main.SetStatus(Tr("Saved {0} bytes to {1}", data.Length.ToString("N0"), path));
                }
            }
            catch (Exception error)
            {
                m_Main.ShowError(Tr("The resource could not be saved"), error.Message);
            }
        }

        // Saves the hex region of a view (section raw data, certificate, overlay).
        private void SaveRegion(
            string node,
            string file_name
        )
        {
            try
            {
                ViewData view = LoadView(node);

                if (view.Hex == null || view.Hex.Length == 0)
                {
                    m_Main.ShowError(Tr("Nothing to save"), Tr("The selected item has no data in the file."));
                    return;
                }

                string? path = FileDialogs.Save(Tr("Save data"), file_name, Tr("Binary files") + " (*.bin)|*.bin|" + ALL_FILES);

                if (path == null)
                {
                    return;
                }

                SaveBytes(view.Hex.Offset, view.Hex.Length, path);
            }
            catch (Exception error)
            {
                m_Main.ShowError(Tr("The data could not be saved"), error.Message);
            }
        }

        public void SaveBytes(
            ulong offset,
            ulong length,
            string path
        )
        {
            using var stream = File.Create(path);
            var buffer = new byte[1 << 20];
            ulong done = 0;

            while (done < length)
            {
                int wanted = (int)Math.Min((ulong)buffer.Length, length - done);
                int read = Document.Read(offset + done, buffer.AsSpan(0, wanted));

                if (read <= 0)
                {
                    break;
                }

                stream.Write(buffer, 0, read);
                done += (ulong)read;
            }

            m_Main.SetStatus(Tr("Saved {0} bytes to {1}", done.ToString("N0"), path));
        }

        private string FileNameWithoutExtension()
        {
            return System.IO.Path.GetFileNameWithoutExtension(m_Path);
        }

        private static string SafeFileName(
            string name
        )
        {
            foreach (char invalid in System.IO.Path.GetInvalidFileNameChars())
            {
                name = name.Replace(invalid, '_');
            }

            return name;
        }
    }
}