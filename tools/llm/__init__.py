"""Provider-agnostic LLM front-end prototype for PTPN.

The LLM is an untrusted proposer: it supplies task metadata, C sources, and
dependency/lock/core declarations.  Everything it returns is validated here
before being compiled, measured, or lowered into a TDG.
"""
