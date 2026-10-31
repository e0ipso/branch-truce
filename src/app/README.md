# Application and editor adapters

ApplicationController connects the window to its collaborators. SessionModel owns the file rows and histories; SelectedFile projects the current comparison. SessionLoader, FileNavigator and ChoicePreviewFlow handle opening, navigation and previews. ResultEditorAdapter translates native text edits and shared actions into durable result revisions. main.cpp configures startup, appearance and mergetool entry.

Add Qt-facing models, controller collaborators and editor/domain translations here. Put visual components in ui/, Git protocols in git/, and background session operations in session/.
