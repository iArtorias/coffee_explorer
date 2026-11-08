using System.Collections.ObjectModel;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Mvvm;

namespace CoffeeExplorer.ViewModels
{
    public sealed class TreeNodeViewModel : ObservableObject
    {
        private bool m_IsExpanded;
        private bool m_IsSelected;

        public TreeNodeViewModel(
            TreeNodeData data,
            TreeNodeViewModel? parent,
            DocumentInfo info
        )
        {
            Id = data.Id;
            Name = data.Name;
            Kind = data.Kind;
            Parent = parent;
            IconKey = ChooseIcon(data, info);

            foreach (TreeNodeData child in data.Children)
            {
                Children.Add(new TreeNodeViewModel(child, this, info));
            }

            // Headers and the tools group start open, long directory lists stay closed.
            m_IsExpanded = Kind == "file" || Id == "nt" || Id == "optional" || Id == "tools" || Id == "dotnet";
        }

        public string Id { get; }

        public string Name { get; }

        public string Kind { get; }

        public string IconKey { get; }

        public TreeNodeViewModel? Parent { get; }

        public ObservableCollection<TreeNodeViewModel> Children { get; } = new();

        public bool IsTool => Kind == "tool";

        // The Tools group and the file node read as headings.
        public bool IsHeading => Kind == "group" || Kind == "file";

        public bool IsExpanded
        {
            get => m_IsExpanded;
            set => SetProperty(ref m_IsExpanded, value);
        }

        public bool IsSelected
        {
            get => m_IsSelected;
            set => SetProperty(ref m_IsSelected, value);
        }

        public void ExpandParents()
        {
            for (TreeNodeViewModel? node = Parent; node != null; node = node.Parent)
            {
                node.IsExpanded = true;
            }
        }

        public TreeNodeViewModel? Find(
            string id
        )
        {
            if (Id == id)
            {
                return this;
            }

            foreach (TreeNodeViewModel child in Children)
            {
                if (child.Find(id) is TreeNodeViewModel found)
                {
                    return found;
                }
            }

            return null;
        }

        private static string ChooseIcon(
            TreeNodeData data,
            DocumentInfo info
        )
        {
            switch (data.Id)
            {
                case "overview":
                    return info.IsDotNet ? "Icon.Assembly" : info.IsPe ? "Icon.BinaryFile" : "Icon.Document";
                case "dos":
                case "nt":
                case "file":
                case "optional":
                case "rich":
                case "dotnet.metadata":
                    return "Icon.Structure";
                case "datadirs":
                    return "Icon.ListView";
                case "sections":
                    return "Icon.Table";
                case "exports":
                    return "Icon.Export";
                case "imports":
                case "delay":
                    return "Icon.Import";
                case "resources":
                    return "Icon.ResourceView";
                case "exceptions":
                    return "Icon.ExceptionPublic";
                case "security":
                    return "Icon.Certificate";
                case "relocs":
                    return "Icon.Link";
                case "debug":
                    return "Icon.Bug";
                case "tls":
                    return "Icon.Thread";
                case "loadconfig":
                    return "Icon.SettingsFile";
                case "bound":
                    return "Icon.Library";
                case "iat":
                    return "Icon.DataTable";
                case "overlay":
                    return "Icon.BinaryFile";
                case "dotnet":
                    return "Icon.Assembly";
                case "dotnet.streams":
                    return "Icon.Package";
                case "dotnet.tables":
                    return "Icon.DataTable";
                case "dotnet.strings":
                case "dotnet.us":
                    return "Icon.String";
                case "dotnet.guid":
                    return "Icon.Key";
                case "dotnet.blob":
                    return "Icon.Binary";
                case "tools":
                    return "Icon.ToolWindow";
                case "tool.address":
                    return "Icon.Calculator";
                case "tool.dependencies":
                    return "Icon.DependencyMatrix";
                case "tool.hex":
                    return "Icon.Binary";
                case "tool.die":
                    return "Icon.Inspect";
                case "tool.importadder":
                    return "Icon.Add";
                case "tool.disassembler":
                    return "Icon.DisassemblyWindow";
                case "tool.rebuilder":
                    return "Icon.BuildSolution";
                case "tool.resources":
                    return "Icon.ResourceView";
            }

            return data.Kind switch
            {
                "folder" => "Icon.FolderClosed",
                "list" => "Icon.ListView",
                "tool" => "Icon.Settings",
                _ => "Icon.Structure"
            };
        }
    }
}