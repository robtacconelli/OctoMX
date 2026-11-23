# Contributing to OctoMX

Thanks for helping. A few rules keep the project healthy and legally clean:

* **Licence.** OctoMX is GPL-3.0-or-later (`LICENSE`, `NOTICE`). By sending a contribution
  you agree that it is published under that licence, and you confirm that you wrote it or have
  the right to submit it under it. New files start with
  `// SPDX-License-Identifier: GPL-3.0-or-later` (`#` for scripts and CMake files).
* **Code from other projects.** Code from GPL-2.0-or-later / GPL-3.0-compatible projects (for
  example eMule, aMule, gtk-gnutella) may be adapted, but say so: name the source file and
  project in a comment at the top of the file and add it to `NOTICE`. Never copy code whose
  licence is incompatible (GPL-2.0-only, proprietary, "no licence"), and never add
  screenshots, icons or other artwork of the original WinMX or other third parties, the UI
  is drawn in code.
* **Test data.** No captured responses of real sites, no real user data, no copyrighted
  titles. Use synthetic fixtures like `tests/web/make_fixtures.py`, and test downloads only with
  clearly legal content (Linux images, Creative Commons / public-domain media).
* **Style.** C++17, Qt 6 (Widgets/Network/Sql), CMake + Ninja. It must build warning-clean-ish
  with GCC (Linux) and clang/llvm-mingw (Windows cross build). No blocking work on the GUI
  thread. Follow the module boundaries in `docs/ARCHITECTURE.md`; the look follows
  `docs/UI_SPEC.md`.
* **Before sending a change:** build both platforms if you can (`docs/BUILDING.md`), run
  `ctest --test-dir <build> --output-on-failure`, add or update tests for what you changed, and
  describe in the pull request what you verified and how. Say plainly what is untested.
* **Privacy.** Tests and tools must never write to `$HOME`; run the app with
  `--profile <dir>` inside the build tree. Do not commit logs, profiles or paths of your machine.
