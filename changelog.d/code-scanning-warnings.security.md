**Command-line paths are resolved before they are used.** `vss_perf --output`
now resolves the path it is given and refuses a directory or a directory that
does not exist, and the fuzz replay driver resolves its input paths and refuses
one that does not exist. The topology canvas also releases its selection rubber
band with the view.
