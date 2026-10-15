# Related work: where P05 sits

This is the working draft of the paper's related-work section. A citation marked **[verify]** has not
been read in full yet. Only its abstract or metadata has been checked, so nothing below may claim
more about it than its abstract does until it is read.

## The comparison

| Tool | Finds parallelism itself? | Interprocedural safety | GPU offload | Offload decision | Output | Soundness |
|---|---|---|---|---|---|---|
| iPat/OMP (2006) | yes, with programmer interaction | [verify] | no (CPU OpenMP) | n/a | OpenMP source | static, user-confirmed |
| Cetus (Purdue) | yes | [verify] (call-site handling) | not in core Cetus. OpenMPC (Lee, Min, Eigenmann, PPoPP 2009) built OpenMP→CUDA on it [verify] | [verify] | OpenMP source | static |
| ROSE AutoPar (LLNL) | yes | via semantic annotations | no | n/a | OpenMP source | static |
| Par4All / PIPS | yes | yes (PIPS is interprocedural) | yes (CUDA/OpenCL) | [verify] | CUDA/OpenCL/OpenMP source | static, polyhedral |
| PPCG (2013) | yes (static-control parts only) | no | yes (CUDA) | none: always maps to GPU | CUDA source | static, polyhedral |
| Polly-ACC (2016) | yes (SCoPs in IR) | no (relies on inlining) [verify] | yes | heuristic, falls back to CPU [verify] | binary, not source | static, polyhedral |
| DiscoPoP | suggests candidates | dynamic (profiling) | not originally; newer versions may [verify] | n/a | hints for the programmer | **unsound** (profile-based) |
| OpenMP Advisor (2023) + data-reuse tool (2020) | **no**: starts from existing kernels | n/a | yes | ML cost model (COMPOFF, MLP), must be trained on the target | OpenMP variants | n/a |
| AutoParLLM / OMPGPT / OMPar (2023–24) | yes | none explicit | mostly CPU pragmas | none | OpenMP source | **no guarantee** (LLM output, validated by tests) |
| **P05 (this work)** | **yes** | **call-site-aware summaries**: a callee's array accesses are substituted with each call's arguments and run through the same dependence tests (recursive or opaque callees make the loop UNKNOWN) | **yes (OpenMP target)**; only for loops whose map extents and pointer disjointness are proven statically | **analytic 3-way GPU/CPU/seq with closed-form runtime guards.** Constants are placeholders until Phase 2 calibration | **standard OpenMP source** | **static, conservative**: pointer overlap is proven impossible (distinct objects, element types, `restrict`, all callers visible) or guarded by a run-time overlap check, else the loop is not parallelized |

The P05 row states where the tool stands now, and labels the Phase 1/2 targets as targets. Update the row as each target lands. Never claim a target before it is done.

## The gap P05 fills (one paragraph for the paper)

Tools that *find* parallelism (Cetus, AutoPar, iPat/OMP) target CPU threads. The GPU-targeted
derivatives such as OpenMPC [verify] translate the code but don't make a priced GPU/CPU/sequential decision
per loop [verify]. Tools that *decide* about offloading (OpenMP Advisor/COMPOFF) assume
the parallel kernel already exists, and they use an ML model whose training mode runs on the target
hardware (per the OpenMP Advisor abstract).
Polyhedral GPU mappers (PPCG, Polly-ACC) are powerful within static-control loop nests, but they reason
about function calls only after inlining [verify]. They emit CUDA or binaries rather than maintainable source.
LLM-based parallelizers give no correctness guarantee. P05 combines static, conservative discovery that
reasons across call boundaries with an explainable, analytic decision between GPU, CPU and sequential.
Its output is standard, human-readable OpenMP. Evaluating it against expert hand-written OpenMP on real
applications shows how close that gets.

**Threat to watch:** Cerberus (github.com/wizardwithcodehazard/Cerberus) is a recent, unpublished
student project that does ML-gated OpenMP offload with a dependence check. It is close in spirit. Per its README it uses an XGBoost model rather than an analytic one, and there is
no paper. Whether it does any interprocedural analysis is [verify]: read the code before claiming it doesn't. Cite it if it is published before submission.

## Reading list (priority order)

1. **iPat/OMP.** M. Ishihara, H. Honda, M. Sato. *Development and Implementation of an Interactive
   Parallelization Assistance Tool for OpenMP: iPat/OMP.* IEICE Trans. Inf. & Syst. E89-D(2):399–407,
   2006. doi:10.1093/ietisy/e89-d.2.399. Suggested by J. Badwaik. Read it in full: which dependence
   tests does it use, and does it handle calls?
2. **OpenMP Advisor.** A. Mishra, A. M. Malik, M. Lin, B. Chapman. arXiv:2301.03636, 2023 (also in
   Springer LNCS, doi:10.1007/978-3-031-40744-4_3 [verify venue]). This is the closest work on the
   offload decision.
3. **COMPOFF.** Mishra et al. *A Compiler Cost Model using Machine Learning to Predict the Cost of
   OpenMP Offloading.* IEEE IPDPSW 2022 [verify], doi:10.1109/... (ieeexplore 9835539).
4. **Data transfer and reuse analysis.** Mishra, Malik, Chapman. *Data Transfer and Reuse Analysis
   Tool for GPU-Offloading Using OpenMP.* 2020 [verify venue: IWOMP?]. This is directly relevant to
   P05's data-region hoisting (Phase 2), which must be positioned against it.
5. **Polly-ACC.** T. Grosser, T. Hoefler. *Polly-ACC: Transparent Compilation to Heterogeneous
   Hardware.* ICS 2016. doi:10.1145/2925426.2926286.
6. **PPCG.** S. Verdoolaege et al. *Polyhedral Parallel Code Generation for CUDA.* ACM TACO 9(4):54,
   2013. doi:10.1145/2400682.2400713.
7. **Cetus.** C. Dave, H. Bae, S.-J. Min, S. Lee, R. Eigenmann, S. Midkiff. *Cetus: A Source-to-Source
   Compiler Infrastructure for Multicores.* IEEE Computer 42(12), 2009 [verify authors].
8. **ROSE AutoPar.** C. Liao, D. Quinlan, J. Willcock, T. Panas. *Semantic-Aware Automatic
   Parallelization of Modern Applications Using High-Level Abstractions.* IJPP, 2010. Also IWOMP 2009.
9. **Par4All / PIPS.** github.com/Par4All/par4all. Find the canonical paper [verify].
10. **DiscoPoP.** Z. Li, R. Atre, Z. Ul-Huda, A. Jannesari, F. Wolf. *DiscoPoP: A Profiling Tool to
    Identify Parallelization Opportunities.* Springer, 2015 [verify]. Also *Unveiling parallelization
    opportunities in sequential programs*, JSS 2016.
11. **LLM-based parallelizers:**
    - AutoParLLM (Mahmud et al., arXiv:2310.04047)
    - OMPGPT (Chen et al., Euro-Par 2024 [verify], arXiv:2401.16445)
    - OMPar (Kadosh et al., arXiv:2409.14771, 2024)
    - Learning to Parallelize with OpenMP by Augmented Heterogeneous AST (MLSys 2023)
12. **Background textbook:** R. Allen, K. Kennedy. *Optimizing Compilers for Modern Architectures.*
    Morgan Kaufmann, 2001. Covers the dependence tests used in Phase 1: ZIV/SIV/GCD/Banerjee.

## Comparators planned for the evaluation (none run yet)

These comparators should be runnable on our hardware:
- **Polly**: `clang -O3 -mllvm -polly -mllvm -polly-parallel`, linked against the OpenMP runtime (`-fopenmp`/`-lomp`), CPU only. Polly-ACC was removed from
  LLVM [verify], so no GPU comparator is available from Polly.
- **GCC**: `gcc -O2 -ftree-parallelize-loops=N`, CPU.
- **Expert hand-written OpenMP** that ships with PolyBench-ACC / XSBench / CoMD. This is the main
  yardstick.
- **P05 `-policy naive`**: the ablation baseline.

Not run: Cetus, Par4All and the LLM tools. They are hard to install reproducibly, or they need
weights and GPUs. These are discussed qualitatively only, and the paper says so.
