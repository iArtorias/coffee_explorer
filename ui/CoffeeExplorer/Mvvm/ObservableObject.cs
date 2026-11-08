using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Input;

namespace CoffeeExplorer.Mvvm
{
    public abstract class ObservableObject : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;

        protected bool SetProperty<T>(
            ref T field,
            T value,
            [CallerMemberName] string? name = null
        )
        {
            if (EqualityComparer<T>.Default.Equals(field, value))
            {
                return false;
            }

            field = value;
            OnPropertyChanged(name);
            return true;
        }

        protected void OnPropertyChanged(
            [CallerMemberName] string? name = null
        )
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }
    }

    public sealed class RelayCommand : ICommand
    {
        private readonly Action<object?> m_Execute;
        private readonly Func<object?, bool>? m_CanExecute;

        public RelayCommand(
            Action<object?> execute,
            Func<object?, bool>? can_execute = null
        )
        {
            m_Execute = execute;
            m_CanExecute = can_execute;
        }

        public RelayCommand(
            Action execute,
            Func<bool>? can_execute = null
        )
            : this(_ => execute(), can_execute == null ? null : _ => can_execute())
        {
        }

        public RelayCommand(
            Action<object?> execute,
            Func<bool> can_execute
        )
            : this(execute, _ => can_execute())
        {
        }

        public event EventHandler? CanExecuteChanged
        {
            add => CommandManager.RequerySuggested += value;
            remove => CommandManager.RequerySuggested -= value;
        }

        public bool CanExecute(
            object? parameter
        )
        {
            return m_CanExecute == null || m_CanExecute(parameter);
        }

        public void Execute(
            object? parameter
        )
        {
            m_Execute(parameter);
        }
    }
}