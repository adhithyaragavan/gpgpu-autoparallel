You are given a complete, sequential C source file. Parallelize it with OpenMP for a multi-core CPU.

Add `#pragma omp parallel for` directives, with any clauses they need (such as `private`, `reduction` or `collapse`), to every loop that can safely run its iterations in parallel. Leave a loop without a pragma if parallelizing it could change the program's results.

Rules:
- Only insert lines that start with `#pragma omp`. Do not change, delete, reorder or reformat any existing line, and do not add any other code.
- Do not use `target` or other GPU directives.
- Return the whole modified file in a single ```c code block and nothing else.

The file:

```c
{{SOURCE}}
```
