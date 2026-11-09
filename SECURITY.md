# Security Policy

## Reporting a vulnerability

Please **do not** open a public issue for a security problem.

Report it privately through GitHub: open the repository's **Security** tab
and choose **Report a vulnerability**. Include what you found, how to
reproduce it, and which Forge commit or version you tested.

A dedicated security email address will be added here once the project
domain is set up.

## What to expect

These are targets, not guarantees: Forge is maintained by one person today.

| Step | Target |
|---|---|
| Acknowledge your report | within 72 hours |
| Fix a critical issue | within 14 days |
| Fix an issue that is being exploited in the wild | within 72 hours |
| Merge a SpiderMonkey security fix | within 72 hours of Mozilla's release |

You will be credited in the release notes unless you ask not to be.

## Supported versions

Forge has not had a release yet. Only the latest commit on `main` gets
security fixes.

## What Forge does NOT protect against yet

Forge's security model (Kavach: per-package permissions, compartments per
package, an OS sandbox) is planned but **not built yet**. Today:

- Every script has full access to the files and network that the user
  running `forge.exe` has.
- There is no isolation between packages, and no protection against bugs in
  the JavaScript engine itself or against Spectre-style side channels.
- The SpiderMonkey engine Forge is built on is a Nightly snapshot, not yet a
  Mozilla security-supported ESR branch (moving to ESR is the next phase).

Reports about these missing features are welcome, but they are known gaps,
not vulnerabilities.
