# Security Policy

## Supported versions

Only the latest code on the `main` branch and the latest tagged release receive security fixes.

| Version | Supported |
| ------- | --------- |
| main and latest release | Yes |
| older releases | No |

## Report a vulnerability

Do not open a public issue for a security problem.

Use one of these channels:

1. GitHub private vulnerability reporting: open the **Security** tab of this repository and select **Report a vulnerability**.
2. Email: brian@heyitsbrian.com with the subject line `SECURITY: tab5-cyberdeck`.

Include:

- The version, release, or commit you tested
- Steps to reproduce the problem
- The effect of the problem (what an attacker can do)
- A proof of concept, if you have one

## What to expect

- I will confirm receipt within 5 business days.
- I will give a first assessment within 14 days.
- I will tell you when a fix is released and credit you in the release notes, unless you ask me not to.

## Scope

In scope:

- Firmware source in `main/`, `components/`, and `apps/`
- The web flasher in `web/` and the GitHub Pages deployment workflow
- Build and release tooling in `tools/`

Out of scope:

- Problems in third-party dependencies. Report those to the upstream project. When a fix is available, I will update the dependency.
- Problems that need physical access to the device that runs this software.
- Denial of service by resource exhaustion on a device that is already on the attacker's network.

## Safe harbor

If you follow this policy and act in good faith, I will not take legal action against you for your research.
