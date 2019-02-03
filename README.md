executable_diff is a command line utility that attempts to tell you
how two executables differ from each other.

Usage:

   ./executable_diff path/to/executable_1 path/to/executable_2

When run, executable_diff will use otool (under MacOS/X) or
objdump (under Linux) to generate a disassembly of each of
the two executables, and then compare each function in executable_1
against its counterpart in executable_2.

executable_diff will then print to stdout a list of the functions
that it found to be different, and generate a .txt file containing
the actual diffs, in case you'd like to see how the assembly differed.

executable_diff is intended to be used to compare slightly varying
versions of the same basic program; obviously if you try to compare
different programs it will report that just about everything is
different, which wouldn't be very useful.

executable_diff tries to minimize false-positives by replacing
absolute addresses with simplified relative addresses, so that
e.g. simple changes in function-sizes won't be reported as diffs,
despite the fact that they result in different addresses being
generated in the assembly.

Note that executable_diff produces only a rought estimate of what
has changed, and is likely to report false-positives in some cases
(and fail to report actual diffs in others).  Please do not use it
as a substitute for version-control and source-code diffs; it is
intended only as a better-than-nothing way to roughly compare 
two executables.

Jeremy Friesner
jfriesne@gmail.com
2/2/2019
