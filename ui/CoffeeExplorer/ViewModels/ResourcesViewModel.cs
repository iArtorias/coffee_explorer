using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Mvvm;
using CoffeeExplorer.Services;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.ViewModels
{
    public sealed class ResourceItemViewModel : ObservableObject
    {
        private bool m_IsSelected;

        public ResourceItemViewModel(
            ResourceData data
        )
        {
            Data = data;
            Index = int.TryParse(data.Path, out int index) ? index : -1;
        }

        public ResourceData Data { get; }

        public int Index { get; }

        public string Caption => string.IsNullOrEmpty(Data.LanguageName) ? Data.Name : $"{Data.Name}  [{Data.LanguageName}]";

        public string ToolTip => Tr("{0} / {1} / {2}, {3} bytes, RVA {4}", TrText(Data.TypeName), Data.Name, Data.Language, Data.Size, Data.Rva);

        public string IconKey => Data.Preview switch
        {
            "icon" => "Icon.IconFile",
            "cursor" => "Icon.CursorFile",
            "image" => "Icon.Image",
            "version" => "Icon.VersionInformation",
            "strings" => "Icon.String",
            "text" => Data.TypeName.Contains("Manifest", StringComparison.OrdinalIgnoreCase) ? "Icon.ManifestFile" : "Icon.TextFile",
            _ => Data.TypeName.Contains("Dialog", StringComparison.OrdinalIgnoreCase) ? "Icon.Dialog" : "Icon.BinaryFile"
        };

        public bool IsSelected
        {
            get => m_IsSelected;
            set => SetProperty(ref m_IsSelected, value);
        }

        // Leaves never expand, the tree style binds the property on every item.
        public bool IsExpanded
        {
            get => false;
            set
            {
            }
        }
    }

    public sealed class ResourceGroupViewModel : ObservableObject
    {
        private bool m_IsExpanded;
        private bool m_IsSelected;

        public ResourceGroupViewModel(
            string name,
            IEnumerable<ResourceItemViewModel> items
        )
        {
            Name = name;
            Items = new ObservableCollection<ResourceItemViewModel>(items);
        }

        public string Name { get; }

        public string Caption => $"{LanguageService.TrText(Name)} ({Items.Count})";

        public ObservableCollection<ResourceItemViewModel> Items { get; }

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
    }

    // 'Resource Directory' and 'Resource Editor'. Types on the left, the selected resource with a typed preview on the right.
    public sealed class ResourcesViewModel : ObservableObject
    {
        private readonly DocumentViewModel m_Owner;
        private ResourceItemViewModel? m_SelectedItem;
        private NodeViewModel? m_Item;
        private string? m_Error;

        public ResourcesViewModel(
            DocumentViewModel owner,
            ViewData data
        )
        {
            m_Owner = owner;
            Data = data;

            var items = (data.Resources ?? new List<ResourceData>()).Select(resource => new ResourceItemViewModel(resource)).ToList();

            foreach (IGrouping<string, ResourceItemViewModel> group in items.GroupBy(item => item.Data.TypeName))
            {
                Groups.Add(new ResourceGroupViewModel(group.Key, group));
            }

            Commands = (data.Commands ?? new List<CommandData>()).Select(command => new CommandItem(command, RunCommand)).ToList();
            SelectCommand = new RelayCommand(parameter =>
            {
                if (parameter is ResourceItemViewModel item)
                {
                    SelectedItem = item;
                }
            });
            ExpandAllCommand = new RelayCommand(() => ExpandAll(true));
            CollapseAllCommand = new RelayCommand(() => ExpandAll(false));
        }

        public RelayCommand SelectCommand { get; }

        public RelayCommand ExpandAllCommand { get; }

        public RelayCommand CollapseAllCommand { get; }

        public ViewData Data { get; }

        public string Title => Data.Title;

        public ObservableCollection<ResourceGroupViewModel> Groups { get; } = new();

        public bool IsEmpty => Groups.Count == 0;

        public List<CommandItem> Commands { get; }

        public List<CommandItem> ViewCommands => Commands.Where(command => !command.IsRowCommand).ToList();

        public List<CommandItem> ItemCommands => Commands.Where(command => command.IsRowCommand).ToList();

        public ResourceItemViewModel? SelectedItem
        {
            get => m_SelectedItem;
            set
            {
                if (SetProperty(ref m_SelectedItem, value))
                {
                    LoadItem();
                }
            }
        }

        public NodeViewModel? Item
        {
            get => m_Item;
            private set => SetProperty(ref m_Item, value);
        }

        public string? Error
        {
            get => m_Error;
            private set => SetProperty(ref m_Error, value);
        }

        public void SelectByIndex(
            int index
        )
        {
            foreach (ResourceGroupViewModel group in Groups)
            {
                foreach (ResourceItemViewModel item in group.Items)
                {
                    if (item.Index == index)
                    {
                        group.IsExpanded = true;
                        item.IsSelected = true;
                        SelectedItem = item;
                        return;
                    }
                }
            }
        }

        public void ExpandAll(
            bool expand
        )
        {
            foreach (ResourceGroupViewModel group in Groups)
            {
                group.IsExpanded = expand;
            }
        }

        private void LoadItem()
        {
            if (m_SelectedItem == null)
            {
                Item = null;
                return;
            }

            try
            {
                Item = new NodeViewModel(m_Owner, m_Owner.LoadView("resources/" + m_SelectedItem.Data.Path), 1);
                Error = null;
            }
            catch (Exception error)
            {
                Item = null;
                Error = error.Message;
            }
        }

        private void RunCommand(
            CommandData command
        )
        {
            m_Owner.RunResourceCommand(command, m_SelectedItem);
        }
    }
}