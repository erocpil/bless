# Security Policy

## Reporting a Vulnerability

Please report security issues to the project maintainers via GitHub Issues.

**Response time:** we aim to acknowledge reports within 72 hours and provide an
initial assessment within 7 days.

## Scope

- Vulnerability reports related to the Bless binary, build system, and runtime
  behaviour.
- Supply-chain concerns in vendored third-party dependencies.

## Out of scope

- Issues that require unrestricted local access to the host (Bless is not a
  privilege-escalation boundary).
- Denial-of-service caused by configuration that saturates the host's own
  network link (this is the tool's intended function).

## Supported versions

| Version | Supported          |
|---------|--------------------|
| main    | :white_check_mark: |
| < 1.0   | :x:                |

## Disclosure

We request a 90-day embargo before public disclosure. Coordinated disclosure
encouraged.
