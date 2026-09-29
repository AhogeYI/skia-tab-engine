# Security Policy

## Reporting a vulnerability

**Please do not open public issues, pull requests, or discussions for
security problems.**

Private channels (either is fine):

- Email `ahogeyi.dev@outlook.com` with a `[security]` subject prefix;
- The GitHub repository's **Security → Report a vulnerability** (private
  security advisory), once the public remote exists.

Please include what you can: the affected version or commit, reproduction
steps, the impact surface (for example, whether it requires a crafted
application embedding the SDK, a specific Windows version, or GPU-specific
behavior), and your preferred disclosure timeline. We acknowledge reports
within 7 days and share the disclosure plan when a fix is available; reporters
are credited when coordination allows.

## Scope and versions

This repository contains the TabEngine source and the SDK build tooling. The
attack surface to consider includes: the Win32 platform layer (message
handling, drag loops, window creation), the D3D12 renderer and swapchain
handling, and anything an embedding application delegates to TabEngine.

Only the latest state of the default branch is maintained; earlier snapshots
and previously published SDK archives do not receive separate security
updates. Applications embedding the SDK should move to a fixed SDK package as
a whole — matching `skia.dll`, import library, and headers — rather than
patching files inside an extracted package.
