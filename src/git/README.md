# Repository inspection and Git operations

GitAdapter supervises bounded Git requests. Its private process, phase and policy files separate execution from operation rules. RepositoryDiscovery verifies source commits and paths; ConflictClassifier and TextConversion decide which files and conversions the app can support.

Add named Git operations, repository parsing and source verification here. Keep GitAdapterState and GitParsing private to this subsystem; result publication coordination belongs in publication/.
