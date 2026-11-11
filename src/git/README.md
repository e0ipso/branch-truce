# Repository inspection and Git operations

GitAdapter supervises bounded Git requests. GitOperations describes each named operation once: its operands, the policy checks before it and the steps it runs. The private process, phase and policy files carry out those steps. RepositoryDiscovery verifies source commits and paths; ConflictClassifier and TextConversion decide which files and conversions the app can support.

Add a named Git operation as one entry in GitOperations' table, and repository parsing and source verification here. Keep GitAdapterState, GitOperations and GitParsing private to this subsystem; result publication coordination belongs in publication/.
