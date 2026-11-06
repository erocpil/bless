# Security Policy

## Supported versions

| Version                      | Supported          |
|------------------------------|--------------------|
| Latest commit on `main`      | :white_check_mark: |
| Latest tagged preview        | :white_check_mark: |
| Older commits / tags         | Best effort        |

Only the latest release receives security patches.

## Scope

Security issues include, but are not limited to:

* Remote code execution through the HTTP/WebSocket control plane.
* Denial-of-service via crafted YAML configuration or WebSocket messages.
* Memory corruption reachable through packet-construction paths.
* Information disclosure in Prometheus metrics, entropy dashboards, or log output.
* Privilege escalation from DPDK secondary processes or hugepage configuration.

The following are **not** in scope:

* Local DoS through resource exhaustion (e.g., allocating all hugepages).
* Misconfiguration of DPDK EAL parameters.
* Vulnerabilities in DPDK, civetweb, libyaml, or other third-party dependencies
  — please report those upstream.

## Authorised use

Bless generates high-rate traffic and malformed packets. Only use it on
systems and networks you own or are expressly authorised to test.
Unauthorised use may violate computer fraud and abuse laws.

## Reporting a vulnerability

**Do not open a public GitHub Issue for security vulnerabilities.**

Report vulnerabilities privately to the repository maintainers:

1. Go to **Security** → **Advisories** on the repository page.
2. Click **New draft security advisory**.
3. Describe the vulnerability, affected versions, and steps to reproduce.

We aim to acknowledge your report within **5 business days** and provide
an initial assessment within **10 business days**.  Once a fix is ready,
we will coordinate disclosure timing with you.

## Disclosure policy

* Critical vulnerabilities: patch released within 7 days of confirmation.
* High-severity: patch within 30 days.
* Medium/low: addressed in the next regular release cycle.

Credit is given to reporters in the advisory and release notes unless
you request anonymity.
