# Problems

A Problem is a typed failure: a code and the values its sentence names, with the problem it wraps. Storage, Git, recovery, the session coordinator and the mergetool return one instead of a sentence, and result types carry it as a value. ProblemText holds the one translated sentence of each code; whatever shows a problem to a person calls it.

Add a code to Problem.h and its sentence to ProblemText.cpp in the same change. Nothing here reads files, runs Git or knows the window.
