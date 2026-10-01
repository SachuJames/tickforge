# Security Policy

## Supported versions

TickForge is in early development (v0.1.0). Security fixes are provided for the latest release only.

| Version | Supported |
|---------|-----------|
| 0.1.x   | yes       |
| < 0.1.0 | no        |

## Reporting a vulnerability

**Do not open a public issue for a suspected vulnerability.**

Use GitHub's private vulnerability reporting on the repository's Security tab. Include:

* A description of the issue and its potential impact.
* Steps to reproduce, if applicable.
* The TickForge version, compiler, and OS.

You can expect an initial response within 7 days. If the report is confirmed, a fix will be prepared and you will be credited in the release notes unless you ask not to be.

## Scope notes

TickForge is a local simulation library: it opens no network ports, runs no services, and executes no code from data files (only structured market events it parses). The most relevant threat surface is malformed input data, which the parser layer is required to reject with reason codes rather than trust (see SPEC.md section 11).
