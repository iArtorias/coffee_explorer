using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text.Json;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Mvvm;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.ViewModels
{
    public sealed class ImportedFunction
    {
        public string Name { get; init; } = string.Empty;

        public uint Ordinal { get; init; }

        public uint Hint { get; init; }

        public string Display => Name.Length > 0 ? Name : Tr("Ordinal {0}", Ordinal);
    }

    // One module of the dependency tree. Children are resolved when the node is expanded, so deep trees of
    // system dynamic libraries cost nothing until the user looks at them.
    public sealed class ModuleNode : ObservableObject
    {
        private readonly DependencyWalkerViewModel m_Owner;
        private bool m_IsExpanded;
        private bool m_IsSelected;
        private bool m_Loaded;

        public ModuleNode(
            DependencyWalkerViewModel owner,
            ModuleNode? parent,
            string name,
            string path,
            bool found,
            string how,
            bool delay,
            List<ImportedFunction> functions
        )
        {
            m_Owner = owner;
            Parent = parent;
            Name = name;
            Path = path;
            IsFound = found;
            How = how;
            IsDelay = delay;
            Functions = functions;
            IsCycle = parent != null && found && Ancestors().Any(ancestor => string.Equals(ancestor.Path, path, StringComparison.OrdinalIgnoreCase));

            // A placeholder child makes the expander visible until the real children are loaded.
            if (found && !IsCycle)
            {
                Children.Add(Placeholder);
            }
            else
            {
                m_Loaded = true;
            }
        }

        private static readonly ModuleNode Placeholder = new();

        private ModuleNode()
        {
            m_Owner = null!;
            Name = Tr("Loading...");
            Path = string.Empty;
            How = string.Empty;
            Functions = new List<ImportedFunction>();
            m_Loaded = true;
        }

        public ModuleNode? Parent { get; }

        public string Name { get; }

        public string Path { get; }

        public bool IsFound { get; }

        public string How { get; }

        public bool IsDelay { get; }

        public bool IsCycle { get; }

        // Functions the parent imports from this module.
        public List<ImportedFunction> Functions { get; }

        public ObservableCollection<ModuleNode> Children { get; } = new();

        public string Caption => IsDelay ? Name + "  " + Tr("(delay load)") : Name;

        public string IconKey => !IsFound ? "Icon.StatusError" : IsCycle ? "Icon.Refresh" : Parent == null ? "Icon.BinaryFile" : "Icon.Library";

        public string ToolTip => !IsFound ? Tr("{0} was not found ({1})", Name, TrText(How)) : IsCycle ? Path + "\n" + Tr("Already loaded further up the tree") : Path + "\n" + TrText(How);

        public bool IsExpanded
        {
            get => m_IsExpanded;
            set
            {
                if (SetProperty(ref m_IsExpanded, value) && value)
                {
                    LoadChildren();
                }
            }
        }

        public bool IsSelected
        {
            get => m_IsSelected;
            set
            {
                if (SetProperty(ref m_IsSelected, value) && value && m_Owner != null)
                {
                    m_Owner.SelectedModule = this;
                }
            }
        }

        public IEnumerable<ModuleNode> Ancestors()
        {
            for (ModuleNode? node = Parent; node != null; node = node.Parent)
            {
                yield return node;
            }
        }

        public void LoadChildren()
        {
            if (m_Loaded)
            {
                return;
            }

            m_Loaded = true;
            Children.Clear();

            foreach (ModuleNode child in m_Owner.ResolveImports(this))
            {
                Children.Add(child);
            }
        }
    }

    public sealed class DependencyWalkerViewModel : ToolViewModel, IDocumentTool
    {
        private ModuleNode? m_SelectedModule;
        private TableViewModel? m_ModuleInfo;
        private string m_Status = string.Empty;

        public DependencyWalkerViewModel(
            DocumentViewModel document
        )
            : base(document, Tr("Dependency Walker"), Tr("Imported modules, resolved like the loader does: application folder, System32, Windows and PATH, API sets through the schema of this Windows."))
        {
            Build();
        }

        public ObservableCollection<ModuleNode> Roots { get; } = new();

        public ModuleNode? SelectedModule
        {
            get => m_SelectedModule;
            set
            {
                if (SetProperty(ref m_SelectedModule, value))
                {
                    LoadModuleInfo();
                    OnPropertyChanged(nameof(Functions));
                }
            }
        }

        public List<ImportedFunction> Functions => m_SelectedModule?.Functions ?? new List<ImportedFunction>();

        public TableViewModel? ModuleInfo
        {
            get => m_ModuleInfo;
            private set => SetProperty(ref m_ModuleInfo, value);
        }

        public string Status
        {
            get => m_Status;
            private set => SetProperty(ref m_Status, value);
        }

        public void OnDocumentChanged()
        {
            Build();
        }

        private void Build()
        {
            Roots.Clear();
            var root = new ModuleNode(this, null, Document.FileName, Document.Path, true, Tr("The file"), false, new List<ImportedFunction>());
            Roots.Add(root);
            root.IsExpanded = true;
            root.IsSelected = true;
            SelectedModule = root;
            int missing = root.Children.Count(child => !child.IsFound);
            Status = missing == 0 ? Tr("{0} direct dependencies, all found", root.Children.Count) : Tr("{0} direct dependencies, {1} not found", root.Children.Count, missing);
        }

        // Imports of a module as child nodes. The root reads the open document so unsaved edits count.
        public IEnumerable<ModuleNode> ResolveImports(
            ModuleNode node
        )
        {
            var children = new List<ModuleNode>();
            JsonDocument? answer = null;
            PeDocument? module = null;

            try
            {
                if (node.Parent == null)
                {
                    answer = Core.Query(new Dictionary<string, object> { ["query"] = "imports" });
                }
                else
                {
                    module = PeDocument.Open(node.Path, 0, true);
                    answer = module.Query(new Dictionary<string, object> { ["query"] = "imports" });
                }

                JsonElement root = answer.RootElement;
                bool is64 = CoreJson.GetBool(root, "is64", true);
                string directory = System.IO.Path.GetDirectoryName(Document.Path) ?? string.Empty;

                if (!root.TryGetProperty("modules", out JsonElement modules))
                {
                    return children;
                }

                foreach (JsonElement item in modules.EnumerateArray())
                {
                    string name = CoreJson.GetString(item, "name");
                    var functions = new List<ImportedFunction>();

                    if (item.TryGetProperty("functions", out JsonElement list))
                    {
                        foreach (JsonElement function in list.EnumerateArray())
                        {
                            functions.Add(new ImportedFunction
                            {
                                Name = CoreJson.GetString(function, "name"),
                                Ordinal = (uint)CoreJson.GetUnsigned(function, "ordinal"),
                                Hint = (uint)CoreJson.GetUnsigned(function, "hint")
                            });
                        }
                    }

                    using JsonDocument resolved = PeDocument.QueryStatic(new Dictionary<string, object>
                    {
                        ["query"] = "resolve",
                        ["name"] = name,
                        ["directory"] = directory,
                        ["is64"] = is64
                    });

                    children.Add(new ModuleNode(this, node, name, CoreJson.GetString(resolved.RootElement, "path"), CoreJson.GetBool(resolved.RootElement, "found"),
                                                  CoreJson.GetString(resolved.RootElement, "how"), CoreJson.GetBool(item, "delay"), functions));
                }
            }
            catch (CoreException error)
            {
                Status = $"{node.Name}: {error.Message}";
            }
            finally
            {
                answer?.Dispose();
                module?.Dispose();
            }

            return children;
        }

        private void LoadModuleInfo()
        {
            if (m_SelectedModule == null || !m_SelectedModule.IsFound)
            {
                ModuleInfo = m_SelectedModule == null ? null : TableViewModel.FromFields(null, new List<FieldData>
                {
                    new() { Name = Tr( "Module" ), Value = m_SelectedModule.Name },
                    new() { Name = Tr( "Status" ), Value = Tr( "Not found" ) },
                    new() { Name = Tr( "Search" ), Value = TrText( m_SelectedModule.How ) }
                }, true);
                return;
            }

            try
            {
                ViewData view;

                // Deferred overviews. The module panel has no use for the hashes of every dynamic library that gets selected, they
                // would read each whole file on the UI thread.
                if (m_SelectedModule.Parent == null)
                {
                    view = Document.LoadView("overview", true);
                }
                else
                {
                    using PeDocument module = PeDocument.Open(m_SelectedModule.Path, 0, true);
                    view = CoreJson.Parse<ViewData>(module.QueryText("{\"query\":\"view\",\"node\":\"overview\",\"defer\":true}"));
                }

                var fields = (view.Fields ?? new List<FieldData>()).Where(field => field.Pending == null).ToList();
                fields.Insert(1, new FieldData { Name = Tr("Resolved by"), Value = TrText(m_SelectedModule.How) });
                fields.AddRange(view.Version ?? new List<FieldData>());
                ModuleInfo = TableViewModel.FromFields(null, fields, true);
            }
            catch (CoreException error)
            {
                ModuleInfo = TableViewModel.FromFields(null, new List<FieldData> { new() { Name = Tr("Error"), Value = error.Message } }, true);
            }
        }
    }
}