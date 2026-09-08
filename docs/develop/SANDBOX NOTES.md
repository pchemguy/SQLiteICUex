---
url: https://chatgpt.com/c/6a9fa5c1-00c4-83eb-bd03-f37bad97e3af
---

## 📗 Dependency Installation

Before installing dependencies, inspect the sandbox to determine what is already available, then install only the missing packages using suitable package managers or other available methods. Do not assume `sudo`, unrestricted networking, or writable default cache directories. If one installation method fails, identify the specific cause and try reasonable alternatives, such as adjusting supported package-manager settings (e.g., disabling the package manager’s download sandbox), redirecting caches to a writable directory, using another available package source, installing to a writable prefix, or building from source. Treat restrictions on a particular command, directory, or tool as limitations of that method rather than proof that installation is impossible. Report a blocker only after practical alternatives have been exhausted or when additional authorization is genuinely required. Finally, verify the installation with version checks and an actual import, execution, or compile-and-link test.

## 📗 Pytest

When pytest tests are required, do not infer that pytest is unavailable merely because the bare `pytest` command is missing from `PATH`. Check the active Python with `python -m pytest --version`, inspect alternative available Python interpreters if necessary, and run tests with `python -m pytest`. Install pytest only if it cannot be imported by any appropriate interpreter. Report pytest as unavailable only after checking module availability, interpreter paths, and reasonable permitted execution or installation alternatives.
