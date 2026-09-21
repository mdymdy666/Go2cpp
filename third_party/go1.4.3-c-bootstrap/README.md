# Go 1.4.3 C bootstrap source subset

This directory is a derived, read-only reference subset extracted from the official
Go 1.4.3 source archive. It contains C, headers, assembly, yacc/lex inputs, and
minimal release metadata used by the pre-self-hosting bootstrap toolchain.

It is not a standalone modern Go distribution and is not compiled by Go2Cpp.
The complete archive and full extraction are next to this directory:

- `../go1.4.3.src.tar.gz`
- `../go1.4.3-full/`

Important runtime/compiler roots:

- `src/runtime/`: C/Go/assembly runtime boundary; this subset keeps the C, header
  and assembly files such as `proc.c`, `panic.c`, `malloc.c`, `chan.h`, and
  `asm_amd64.s`.
- `src/cmd/`: the pre-self-hosting C compiler/toolchain sources (`5c`, `6c`,
  `8c`, `gc`, and related headers).

Go 1.4.3 is the final useful bootstrap-era reference before Go 1.5 completed
the compiler/runtime self-hosting transition. Context is not present in this era;
use the later Go context package only for the API-level context comparison.

Source archive SHA-256:
`9947fc705b0b841b5938c48b22dc33e9647ec0752bae66e50278df4f23f64959`
